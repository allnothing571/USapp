# USapp Phase 1 M3-C Design: Task Completion and Reward Settlement

Status (updated 2026-10-14, Asia/Shanghai): On 2026-10-10 the owner authorized M3-C design/planning. On 2026-10-14 the owner approved a lighter testing and acceptance workflow, D30 Option A and the closed-enum D32 hook, deferred D33 (revision 3, Sections 6-13), and then approved the **M3-C1 backend batch** (D36, D37, D38, D39, D41). **M3-C1 is implemented by Claude Code and passed the owner-run full CTest (Section 16.5); it is not accepted.** The owner then approved **M3-C2 (web) including D40**; it is implemented by Claude Code, owner-run automated web checks passed and the owner deployed the backend/web and reported representative live checks successful (Sections 17.5 and 18); no separate formal acceptance or later Git publication is recorded. D33 stays deferred. Every item marked "proposal" is an engineering suggestion, not an owner decision; Section 13 lists the approval status. Revision 2 (2026-10-10, after review): transaction-stage wording for rejections and unknown commits (4.3-4.5, 7-8), a concrete D32 hook (8.1), completion/list/retry interaction on the web (5, 6.4), atomic D33 initialization (8.2), corrected rollback compatibility (11), and the D41 mapping change (10).

Written by Claude Code against the repository state after M3-B (owner-reported commit `380696b`, dated observation): frozen migrations 0006 and 0007, `services/api/src/services/TaskService.*`, `TaskController.cc`, `db/Db.h`, `db/TaskPrivilegePolicy.*`, `WalletService.*`, contract 0.4.0, and the web `api/tasks.ts`, `stores/tasks.ts`, `stores/wallet.ts`, `views/TasksView.vue`. All reads were read-only. The parent design is [phase1-m3-design.md](phase1-m3-design.md) (revision 3; Sections 3.1-4.5 and 6.3 describe completion at proposal level; this document refines them against the actual code).

## 1. Scope

In M3-C:

- The assignee completes an open one-off task once. Completion creates (mints) the task's rewards in the assignee's ledger (D1) and records the completion, atomically.
- Idempotent retries, conflict classification, unknown-commit handling.
- Web: a completion control for tasks assigned to the caller, with manual retry, offline block, account invalidation and a wallet refresh after success.
- Tests: real-service settlement, concurrency on existing accounts, post-commit snapshot visibility, partial-write rollback, cap rejection, committed-settlement append attacks.

Not in M3-C: counting/recurring tasks (M4/M5), edit/cancel (M6), undo (D6: no undo), redemption (M7-M9), notifications, partner balances (D21), admin endpoints, any change to 0006/0007, grants, dependencies, deployment configuration, `Db.h`/`Db.cc`.

## 2. Fixed inputs

### 2.1 Approved decisions (owner) that M3-C must follow

| ID | Decision (as approved) | Source |
| --- | --- | --- |
| D1 | Completion creates new rewards for the recipient; no debit of the publisher. | plan 9.9 |
| D2 | Balance caps 1,000,000 flowers / 1,000,000,000 sunshine; per-entry caps 999 / 100,000; reject, never clamp. | plan 9.9 |
| D21 | Wallet shows own balances only. | plan 9.9 |
| D5/D5a/D5c | Decision time read after account locks and cap check, strictly before `deadline_at`, stored as `completed_at`; later commit delay does not change the decision; success requires a confirmed commit. | m3-design 2.3 |
| D6/D7/D8/D24 | No undo; partner-only publication; both members see tasks; only the assignee completes. | m3-design 2.3 |
| D20/D24a | Completion is idempotent; same-key retry returns the original result; a different key for an already completed task is rejected without another reward. | m3-design 2.3 |
| D22/D22a | Integer rewards 0-999 / 0-100,000, at least one positive (schema enforces). | m3-design 2.3 |
| D27 | Exceeding a balance cap rejects the whole completion; task remains incomplete. | m3-design 2.3 |
| D31/D35 | Completion returns settlement facts, not balances; client reloads `GET /api/v1/wallet`. Unconfirmed outcomes keep the key for an explicit retry. | m3-design 2.3 |
| D26/D34 | Narrowed task grants (SELECT, INSERT only); settlement integrity triggers in 0007. | m3-design 2.3, 0007 |
| Plan 9.8 | Binding transaction and lock rules for every posting path (Section 4.2 below discusses the conflict with D30). | plan 9.8 |

### 2.2 Approval status of the engineering choices

Approved (owner, 2026-10-14): D30 Option A (Section 4.2), the closed-enum D32 hook (Section 8.1), the M3-C1 backend batch with D36, D37, D38, D39 and D41, and the M3-C2 web batch with D40. Deferred: D33. Not authorized and not used: D29 (test-database reset). Any new migration or grant change and any `db_gated` timeout increase still need separate approval.

### 2.3 Frozen schema facts relied on (0006, 0007; verified by reading)

- `wallet_operations`: `UNIQUE (operation_type, source_type, source_id)` (`wallet_operations_source_unique`) and partial unique index `(actor_user_id, operation_type, idempotency_key) WHERE idempotency_key IS NOT NULL`; key 16-128 chars; key and fingerprint both NULL or both set; fingerprint `^[0-9a-f]{64}$`; append-only (UW001); deferred `wallet_operations_require_entries` (UW006).
- `wallet_accounts`: PK `(user_id, asset_type)`, member FK, `lock_token` column-level UPDATE for `usapp_api` (used only for `SELECT ... FOR UPDATE`; any real UPDATE raises UW001).
- `wallet_ledger_entries`: BEFORE INSERT chain trigger locks the account `FOR UPDATE` and checks `seq = prev + 1` and `balance_after = prev + amount` (UW002-UW004); one row per INSERT statement (UW005); `UNIQUE (user_id, asset_type, seq)` and `UNIQUE (operation_id, user_id, asset_type)`; amount and balance CHECKs implement D2.
- 0007 `task_completions`: PK `task_id`, `UNIQUE (operation_id)`, composite FKs; BEFORE INSERT guard (UT002-UT006: task/couple, assignee, reward snapshot, operation type/source/actor/couple, `completed_at <= clock_timestamp()`, `>= created_at`, `< deadline_at`).
- 0007 deferred settlement validator `usapp_task_settlement_validate` (UT007) on `task_completions` insert, on `wallet_operations` insert with `source_type = 'task'`, and on **every** `wallet_ledger_entries` insert (returns early unless the operation's source type is `task`).
- Grants: `usapp_api` has SELECT and INSERT only on `tasks` and `task_completions`, so it **cannot** lock a `tasks` row (`FOR UPDATE/SHARE` needs UPDATE privilege on at least one column); it can lock `couple_members` and `wallet_accounts`.
- Drogon v1.9.11 delivers SQL errors without SQLSTATE; a failed commit is only `false` in the commit callback.

## 3. Contract proposal (OpenAPI 0.5.0, additive) - proposal D37

All existing operations, schemas and error codes of 0.4.0 are kept unchanged.

`POST /api/v1/tasks/{taskId}/completion` (`completeTask`), behind `AuthFilter` (Origin + session-bound CSRF + formal session):

- Header `Idempotency-Key` (same rule as publish: 16-128 `[A-Za-z0-9_-]`); request body must be absent or exactly `{}` (any field is 400).
- 200 `{ "task": Task, "settlement": { "operation_id": uuid, "flower": int, "sunshine": int } }` for the first confirmed completion **and** for a same-key replay (identical body). `task.status` is `completed`; `completed_at` is the stored decision time (six-digit UTC). `settlement.flower`/`sunshine` are the credited amounts (0 for an unrewarded asset). **No balance fields.**
- Errors (new codes are added to `ErrorEnvelope`, `core/Errors.*` and web `error-messages.ts`):

| Code | HTTP | When |
| --- | --- | --- |
| `VALIDATION_ERROR` | 400 | Missing/malformed key, non-empty body |
| `UNAUTHENTICATED` | 401 | No formal session |
| `CSRF_TOKEN_INVALID` / `ORIGIN_NOT_ALLOWED` | 403 | Existing filter |
| `SPACE_NOT_FOUND` | 404 | Caller has no space |
| `TASK_NOT_FOUND` | 404 | Unknown id or another space's task (indistinguishable) |
| `TASK_NOT_ASSIGNEE` (new) | 403 | Caller is a member but not the assignee (includes the publisher) |
| `TASK_ALREADY_COMPLETED` (new) | 409 | Task already settled by another key |
| `IDEMPOTENCY_KEY_REUSED` | 409 | Same key, different fingerprint (for example another task) |
| `TASK_EXPIRED` (new) | 409 | Decision time not before the deadline |
| `WALLET_BALANCE_LIMIT` (new) | 409 | Credit would exceed a D2 balance cap |
| `RESULT_UNKNOWN` | 503 | Commit outcome could not be confirmed; retry with the same key |
| `INTERNAL_ERROR` | 500 | Confirmed failure before COMMIT (SQL error) |

Status-code choice (proposal): 200 for both first completion and replay (the parent design's choice), because the resource is the task's completion state; an alternative is 201 for first completion. Either is additive.

## 4. Settlement transaction (exact proposed order)

### 4.1 Pure pre-checks (no database)

1. `requireUser`; key validation; body must be empty or `{}`; task id must look like a uuid (otherwise 404 `TASK_NOT_FOUND`, as in M3-B).
2. Fingerprint: lowercase SHA-256 of `complete-task:v1:` + `task_id=<len>:<lowercase task uuid>;` (same length-prefixed style as publish).

### 4.2 Lock order and the plan 9.8 conflict (D30 Option A, approved direction)

Plan 9.8 rule 2 (binding) requires: business row -> `couple_members` (FOR SHARE) -> operation insert -> accounts sorted. With the frozen D26 grants, `usapp_api` cannot lock a `tasks` row. Therefore M3-C implementation needs **one** of:

- **Option A (approved direction, recorded 2026-10-14).** No task-row lock. Order: `couple_members` FOR SHARE (with the task read) -> operation claim -> accounts ascending `(user_id, asset_type)` -> decision. Justification: tasks are immutable in M3 (no state to protect); the business-source unique key `(TASK_REWARD, 'task', task_id)` serializes completions of one task; 0007 triggers bind completion, operation and entries. Must be revisited when M6 makes tasks editable.
- **Option B (not chosen): a new migration** granting a lock-only column privilege on `tasks` (like `wallet_accounts.lock_token`), plus `init-roles.sql` and `TaskPrivilegePolicy` changes. It would weaken D26 for no M3 benefit.

Everything below assumes Option A. Plan 9.8 rule 1's "update the business row" does not apply (tasks are immutable). Rule 6 (`int64_t` with explicit `::bigint` casts for numeric parameters) is followed for amounts, sequence numbers and balances on this posting path (M3-B read/publish paths bind text with casts; posting uses the binding rule).

### 4.3 Steps (one `TxScope`, READ COMMITTED)

Terminology used below. **Pre-COMMIT rejection**: a business decision (4xx) or SQL error before COMMIT is requested. Earlier statements of the same transaction may already have written temporary rows (for example the claimed operation, accounts, the completion or the first entry); they are visible only inside that transaction. The service ends such a transaction by requesting rollback (`TxScope::rollback` or `TxScope::fail`) and then responds. The rollback callback confirms only that the rollback was **requested** (Drogon v1.9.11 has no rollback completion notice); it is not evidence that the server finished rolling back or released locks. Tests therefore observe separately: server-side transaction termination and lock release (`pg_stat_activity`/`pg_locks` polling), and absence of committed rows (observer connection). **Unconfirmed COMMIT**: the commit callback reports `false`; the outcome is unknown (Section 4.5), never reported as failure or as "nothing settled".

1. **Task and membership read**, one statement: the caller's `couple_members` row `FOR SHARE OF m`, left-joined to the task in the same couple and its completion row. No membership row: 404 `SPACE_NOT_FOUND`. No task: 404 `TASK_NOT_FOUND`. Caller not the assignee: 403 `TASK_NOT_ASSIGNEE`. (No deadline or completion check here; those come later so that a same-key replay is never rejected for a later state.)
2. **Claim**: `INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint) VALUES ($id, couple, caller, 'TASK_REWARD', 'task', task_id, key, fp) ON CONFLICT DO NOTHING RETURNING id`, operation id generated in C++. `ON CONFLICT DO NOTHING` without a target covers both the source key and the partial key index; an in-progress conflicting insert makes this statement wait, then skip (other committed) or insert (other rolled back).
3. **No row returned -> classify** (never by SQLSTATE; this transaction has written nothing yet):
   - `SELECT ... FROM wallet_operations WHERE actor_user_id = caller AND operation_type = 'TASK_REWARD' AND idempotency_key = key`: found with the same fingerprint -> **replay** (read the completion, task and operation; roll back; 200 with the rebuilt body); found with a different fingerprint -> roll back, 409 `IDEMPOTENCY_KEY_REUSED`.
   - Not found by key: `SELECT 1 FROM wallet_operations WHERE operation_type = 'TASK_REWARD' AND source_type = 'task' AND source_id = task_id`: found -> roll back, 409 `TASK_ALREADY_COMPLETED`; not found -> roll back, 500 (should not happen: DO NOTHING only skips after a committed conflict; the other unique constraint is the random primary key).
   - A replay whose operation exists but whose completion row is missing cannot be committed (0007 validator), so a missing row is a 500, not a replay.
4. **Accounts**, for each positive-reward asset in ascending `(user_id, asset_type)` order (one user, so `flower` before `sunshine`): `INSERT INTO wallet_accounts (user_id, asset_type, couple_id) VALUES (...) ON CONFLICT (user_id, asset_type) DO NOTHING`, then `SELECT 1 FROM wallet_accounts WHERE ... FOR UPDATE`. Zero-reward assets are not touched.
5. **Reads after lock acquisition**: for each locked account, read the latest entry (`seq`, `balance_after`, default 0/0) in a statement issued after the lock statement completed (new READ COMMITTED snapshot, so a commit by the previous lock holder is visible).
6. **Bounds**: if `balance + reward` exceeds the D2 cap for any asset: pre-COMMIT rejection, 409 `WALLET_BALANCE_LIMIT`. At this point the transaction holds a temporary claimed operation and possibly newly created accounts; rollback is requested, so none of them can commit. The 0006 balance CHECK is the backstop (it would surface as a 500).
7. **Decision and completion insert (proposal D38: one statement)**:
   `WITH n AS (SELECT clock_timestamp() AS t) INSERT INTO task_completions (task_id, couple_id, completed_by, operation_id, reward_flowers, reward_sunshine, completed_at) SELECT t.id, t.couple_id, $caller, $op, t.reward_flowers, t.reward_sunshine, n.t FROM n, tasks t WHERE t.id = $task AND (t.deadline_at IS NULL OR n.t < t.deadline_at) RETURNING completed_at`.
   No row: the task expired at the decision time -> pre-COMMIT rejection (temporary operation/accounts; rollback requested), 409 `TASK_EXPIRED`. The parent design's two-statement form (decision SELECT, then INSERT with the returned value) is the alternative; the single statement avoids passing the timestamp back through text and makes "decision then insert" atomic. The 0007 guard re-checks assignee, snapshot, operation link and `completed_at < deadline_at`.
8. **Entries**: one INSERT per positive asset (`flower`, then `sunshine`), `seq = prev + 1`, `amount = reward`, `balance_after = prev + reward`, all as `int64_t` with `::bigint`. The chain trigger re-verifies under the same account lock.
9. **Audit**: `task.completed`, actor = assignee, couple, target = task id, detail `{"operation_id","flower","sunshine"}` only (no title/description).
10. **Read back** the response facts (task row with status `completed`, completion, operation id) inside the transaction.
11. **Commit**: set `commitRequested = true` immediately before `TxScope::commit`; deferred checks run at COMMIT (UW006, UT007 x3). Success only from the commit callback with `true`. Commit callback `false` (or a failed commit-callback installation) -> 503 `RESULT_UNKNOWN` with a fixed label `task_complete stage=commit reason=commit_unconfirmed`. The service does not claim failure or absence of a settlement in this case: Drogon reports a server-rejected COMMIT (for example a deferred-check failure) and a COMMIT whose result was lost identically.

### 4.4 Deadline semantics (D5a/D5c, approved)

- Admission time is not used. The decision time is the `clock_timestamp()` read by the step-7 statement, which runs after the claim (step 2, may wait), the account locks (step 4, may wait) and the cap check (step 6).
- A completion decided before the deadline may commit after it (D5c). A later statement that fails before COMMIT is a pre-COMMIT rejection (no success response; rollback requested). A `false` commit callback is `RESULT_UNKNOWN`: the completion may or may not have committed, and only a same-key retry (or a read) settles which.
- Replays never re-evaluate the deadline: a same-key retry after the deadline returns the original 200 body.
- Precedence for a new key: already completed (step 3) is reported before expiry (step 7), so "completed then expired" tasks always answer `TASK_ALREADY_COMPLETED` to other keys.

### 4.5 Unknown commit outcomes (D35, approved)

As in M3-B: the client keeps the key after any 5xx (including 503), network error or timeout and retries explicitly with the **same** key. If the original committed, the retry replays it (200, identical body); if it did not, the retry runs the settlement once. Business-source uniqueness makes a second mint impossible either way. A deterministic deferred-check failure would make the retry end in `RESULT_UNKNOWN` again; it is reported to the user, never retried automatically, and the outcome is never presented as a confirmed failure. A 500 is used only for errors before COMMIT was requested.

### 4.6 Replay body (D31, approved)

Built only from immutable rows: `tasks` (immutable in M3), `task_completions` (`completed_at`, snapshot amounts) and the operation id. Never from balances. Because tasks cannot change in M3, the replay body is byte-for-byte identical to the original success body (same JSON field order and six-digit timestamps). A replay must never be used by the client to set balances.

### 4.7 Callback ownership and exactly-once completion

Use the repaired M3-B `TaskService` pattern unchanged in shape:

- A heap `CompleteCtx` owns its `TxScope`; the error handler stored in `TxScope::onError_` captures the context **weakly**; SQL and commit callbacks capture it strongly; all end paths go through one `complete()` guarded by a `completed` flag; `done` is released with the context.
- Failure details are fixed labels from `TaskService::failureDetail` style (`task_complete stage=<claim|classify|accounts|balances|decision|entry|audit|read_back|commit|transaction> reason=<...>`); the ORM exception object and the transaction callback's message are never read or logged by this service.
- `ctx->scope` is never reset while a `TxScope` method runs. No change to `Db.h`/`Db.cc`.
- Proposed placement (D41): a new `TaskCompletionService` (`services/TaskCompletionService.h/.cc`) reusing `TaskService` helpers, to keep `TaskService.cc` from growing further. `taskJson`, `validIdempotencyKey` and `failureDetail` are already public. The row mapper `taskFromRow` and the shared column list `kTaskColumns` are currently internal to `TaskService.cc`; D41 therefore also modifies `TaskService.h/.cc` to expose them unchanged as public statics, keeping the existing signature and behaviour `TaskService::taskFromRow(const drogon::orm::Row &row, bool withHistory = false)` (history callers keep passing `withHistory = true`, which populates `historyAt`; all other callers keep the default) and `TaskService::taskColumnsSql()`, so the completion read-back and replay use exactly the M3-B column list, timestamp format and status rule. GET/list/publish SQL texts and behaviour stay identical (regression coverage in 6.2 and 6.3 R9). Alternative: add `completeTask` to `TaskService` itself (no exposure needed, larger file).

## 5. Frontend design

- `api/tasks.ts`: `completeTask(taskId, key)` -> `POST /api/v1/tasks/{taskId}/completion` with `Idempotency-Key`, empty body, shared client (CSRF, Origin, cookies, offline block).
- `stores/tasks.ts`: per-task completion state `{ key, status: 'submitting' | 'unknown' | 'error', errorText }`, memory only. One key per click on an open task; the key is reused only by an explicit "retry" for the same task after a network error or 5xx (including `RESULT_UNKNOWN`); discarded after 2xx or 4xx and on `reset()`. No automatic retry or replay; offline is blocked before fetch and never queued.
- Session-generation and user-id guards (as for publish) on every completion success and failure; a late result after logout or account switch is dropped silently (no list change, no notice, no wallet load).
- **Pending completion actions are independent of list membership.** The store keeps `completionActions: Map<taskId, { key, title, rewards, status: 'submitting' | 'unknown' | 'error', errorText }>`, capturing the title and reward snapshot at click time. List refreshes never create, modify or delete these entries; only the action's own response, an explicit dismissal of a definitive error, or `reset()` does.
- **Applied confirmed success (2xx, first completion or replay)**:
  1. Remove the action (its key is discarded) and record a separate `completionNotices` entry ("已完成：+N 小红花 / +N 阳光值", from `settlement`, feedback only).
  2. Remove the task from the open view immediately (local), so it cannot be clicked again.
  3. Reload **open and history from page one**: incrementing each view's request sequence invalidates every older first-page or load-more response, so a stale page cannot re-insert the completed task into open. The task then appears in history from the server's response.
  4. Call `useWalletStore().load()` once.
  The notice, the list reloads and the wallet reload are separate state: a list or wallet reload failure shows its own retryable error and never turns the confirmed completion into a failure. Settlement amounts are never written into wallet balances.
- **Unknown outcome (network error, 5xx, `RESULT_UNKNOWN`)**: the action stays `unknown` with its key. The UI renders unknown actions in their own "结果未确认的完成" panel from the action snapshot, **not** from the open list. If a later list refresh no longer shows the task in open (because the original actually committed, or the task expired), the explicit "重试完成" control (same key) remains available. A retry that returns 200 is a replay of the committed original and is handled as a confirmed success; a definitive 4xx ends the action.
- 409 `TASK_ALREADY_COMPLETED` / `TASK_EXPIRED` (applied): end the action, show the message, and reload open and history from page one.
- `TasksView`: "完成" button only on open tasks where `assignee_id === me` and without a pending action for that task; disabled while submitting; the unknown-actions panel shows each pending action's snapshot and its "重试完成" button (same key); component-local notices are cleared on account change and guarded against unmount (M3-B pattern); `completionActions` and `completionNotices` are cleared by `reset()`.

## 6. Test plan (revision 3, lighter workflow approved by the owner on 2026-10-14)

### 6.1 Principles

- Keep real-service, real-database tests only where they prove settlement behaviour that nothing cheaper can prove (concurrency, locking, deferred triggers, rollback, replay). Fold audit and callback-lifetime assertions into those cases instead of separate fixtures and cases.
- Use small parameterized pure tests for boundaries (rewards, caps, request validation, response shape). Reuse the existing M2-A/M3-A schema tests for constraint and trigger boundaries; do not duplicate them.
- Reuse the existing probe and fixture utilities (`WalletLedgerProbe.h`, `WalletLedgerDbTests.h`, `TaskLedgerDbTests.h`, `TaskServiceDbTests.h` helpers) or small local helpers. No general test framework, no new dependency.
- Do not remove existing tests, weaken the `usapp_test` guard, or change grants, triggers or constraints.
- Static checks only for meaningful boundaries, checked on exact route/call lines or on SQL returned by public functions, not as whole-file keyword scans that can match comments.
- Ordinary wording or layout changes need no new automated test.

### 6.2 Lightweight unit and static tests (M3-C1 and M3-C2)

- Parameterized pure tests: completion fingerprint; `Idempotency-Key` and empty-body rules; reward/cap pre-check arithmetic at the boundaries (balance + reward exactly at, and one above, each D2 cap for both assets); outcome -> HTTP/error-code mapping for the four new codes; response shape (exactly `task` and `settlement`, no balance fields); fixed failure-detail labels; `SettlementFault` default `None`.
- D41 regression: `taskFromRow` keeps `withHistory = false` as default and fills `historyAt` only with `true`; the existing M3-B unit cases for `listSql`, `getSql`, `taskJson` and cursors run unchanged.
- Static (meaningful boundaries only): contract 0.5.0 adds only `completeTask` and the four codes while keeping existing operations; the completion route line is registered with `AuthFilter` and POST only; the controller's completion call does not pass a `SettlementFault` argument; 0006/0007 checksums stay pinned.
- **Explicit limit:** these unit tests and the existing database CHECK tests do **not** prove the real-service near-cap rejection path; that path is deferred with D33 (Section 8.2).

### 6.3 Retained real-service settlement cases (M3-C1, committed D36 fixtures on `usapp_test`)

Each case states its integrated audit and lifetime assertions. "Pre-attempt baseline" means the task's audit rows captured before the attempt; rejection assertions mean no **new** `task.completed` audit for that attempt (existing `task.created` audits are expected).

| # | Case | Key assertions |
| --- | --- | --- |
| R1 | Flower-only, sunshine-only and both-reward settlement (one couple, three tasks) | one operation, one completion, one entry per positive asset with correct `seq`/`balance_after`; exactly one new `task.completed` audit without text; completion, operation, entries and audit share one `xmin`; balances via `WalletService` equal the entry sums; callback called once and released (sentinel) |
| R2 | Same-key replay and key conflicts | same key -> byte-identical body, no new rows or audits; same key on another task -> 409 `IDEMPOTENCY_KEY_REUSED`; other key on a completed task -> 409 `TASK_ALREADY_COMPLETED`; partner reusing the key value is a separate scope; replay path releases its callback (sentinel) |
| R3 | Concurrent completion | different keys -> exactly one 200 and one 409, one operation; same key -> two 200 with identical bodies, one operation, one audit |
| R4 | Authorization | publisher 403 `TASK_NOT_ASSIGNEE`; member of another space 404; user without a space 404; no new rows or `task.completed` audits |
| R5 | D32 partial-write rollback, then same-key retry | `FailAfterFirstEntry` on a both-reward task -> 500; observed transaction end/lock release; no committed operation, entry or completion and no new `task.completed` audit; the same key without the fault settles once; the fault path releases its callback (sentinel) |
| R6 | Existing-account serialization and post-commit visibility | committed account with one committed non-task entry; a holder on a dedicated client holds `FOR UPDATE`; the completion does not finish within ~300 ms and a `Lock` wait is observed; the holder inserts and commits a valid non-task entry, then ends; the completion's entry builds on that committed `seq`/`balance_after` |
| R7 | Deadline across a wait, and replay after the deadline | honest direct-insert task with a seconds-precision deadline (labelled as an API-unproducible row); holder delays the completion: released after the deadline -> 409 `TASK_EXPIRED` with nothing committed and no new `task.completed` audit; released before -> 200 with `completed_at < deadline_at`, and a same-key replay after the deadline returns the identical body |
| R8 | Committed-settlement append attacks (Section 9) | other member's chain-valid entry on the committed operation -> `UT007` at `SET CONSTRAINTS ALL IMMEDIATE`; unrewarded-asset chain-valid entry -> attempted COMMIT reports `false` and the observer finds no committed row; original entry, completion and both balances unchanged |
| R9 | M3-B read regression after settlement (folded into R1) | `getTask` and both list views show the R1 tasks as `completed` with the settlement's `completed_at`, absent from open and present in history |

Error-detail redaction (fixed labels, no echoed input) is asserted inside R4/R5 for the paths they already exercise; no separate case.

### 6.4 Web behaviour tests (M3-C2)

Only these behaviours get new automated tests (`tasks-completion.spec.ts` for the store, additions to `tasks-view.spec.ts` for the view):

1. Manual same-key retry after an unknown result (503 `RESULT_UNKNOWN`, 5xx, network error): the explicit retry sends the same key; a 200 replay is handled as confirmed.
2. A pending retry survives the task's removal from the open list by a refresh, with its key and retry control.
3. Logout and account switch: late completion success/failure is discarded (no list change, no notice, no wallet load) and pending actions are cleared.
4. Offline completion is blocked before fetch and not replayed after `online`.
5. A confirmed completion stays shown as successful when the open/history refresh or the wallet reload fails; the task leaves open and the reloads start from page one (an older stale page cannot re-insert it).

### 6.5 Retained, reduced and deferred coverage

- Retained: everything in 6.3 and 6.4, the pure boundary tests in 6.2, and all existing M2/M3 tests.
- Reduced: rollback-only shape tests of individual SQL statements (covered by R1-R7 on the real path); separate lifetime and redaction cases (folded into R1/R2/R4/R5); the duplicate-asset append case (already rejected by 0006's unique key and covered by M2-A tests).
- Deferred: D33 near-cap real-service rejection and everything belonging to its persistent fixture (initializer, validation, concurrent initializers); the sunshine-cap real-service path; any forced lost-COMMIT test (not reproducible).

## 7. Committed fixtures (D36, retained for the implementation batch; not yet authorized to create)

- Only on literal `usapp_test`, enforced by the existing `current_database()` guard before any write; never on the main database. This plan does not authorize creating fixtures or running database commands; the owner runs the tests.
- Per-run random identifiers; users under `@m3-fixture.test` with prefix `task_m3c_`; assertions filter by this run's identifiers; no global counts.
- No bypass: no DELETE grants, disabled triggers, `SECURITY DEFINER` purge or reset (D29 stays unauthorized). Committed rows are permanent.
- Growth per full `db_gated` run for R1-R9 as implemented (Section 16; R1 and R9 share fixtures): 8 users, 4 couples, 13 tasks, 11 completions. **Operations are counted by source**: each committed completion has exactly one task-sourced operation (`source_type = 'task'`), so 11 task-sourced operations; in addition, R6 commits 2 non-task fixture/holder operations (`source_type = 'wallet_test'`), 13 operations in total. 16 entries (14 task-settlement entries plus 2 fixture/holder entries), 5 accounts, and 22 audit rows (11 `task.created` from real publishes plus 11 `task.completed`). No D33 fixture.
- Bounded waits and teardown as in M3-B: dedicated single-connection holder; bounded acquire/statement/commit/lock-wait/teardown polling; the observer never observes its own transaction.
- Evidence limits: a pre-COMMIT rejection's rollback callback proves only that rollback was requested; tests assert termination/lock release and committed-row absence separately. A `false` COMMIT in a raw test transaction is checked by observing rows.
- Time: R6 and R7 add a few seconds (R7's deadline window dominates); the 30 s `db_gated` timeout is kept, and any increase after measurement needs owner approval.

## 8. Fault hook (D32, approved direction) and cap fixture (D33, deferred)

### 8.1 D32: closed-enum fault hook (direction approved by the owner)

- `enum class SettlementFault { None, FailAfterFirstEntry };` passed by value to `completeTask(userId, taskId, key, cb, SettlementFault fault = SettlementFault::None)` and copied into the completion context; no SQL string, no placeholder.
- With `FailAfterFirstEntry`, immediately after the first entry INSERT the service runs one fixed statement `SELECT 1 / (SELECT count(*) - 1 FROM wallet_ledger_entries WHERE operation_id = $1::uuid)` bound to `ctx.operationId`. Wrong placement (before any entry: `1 / -1`; after both: `1 / 1`) succeeds and makes R5 fail on the unexpected 200; R5 uses a both-reward task.
- Expected outcome and evidence: R5 (Section 6.3).
- Production isolation: the controller's completion call never passes the argument (static check on that call line); default `None`; no HTTP, header, configuration or environment path; the statement is fixed and reads only the service's own operation. Limit: the seam is compiled into the production binary.

### 8.2 D33: deferred

The persistent near-cap fixture (initializer, atomic first-use, validation of an existing fixture, concurrent-initializer serialization, the reserved advisory key) is **not** part of the upcoming implementation batches. The earlier proposal text (revision 2) is superseded and kept only as history in this note. Consequence: no real-service `WALLET_BALANCE_LIMIT` test; the pre-check arithmetic (6.2) and the M2-A CHECK tests cover the boundaries but not the service path. Revisit with a separate approval.

## 9. Committed-settlement append attacks (D34 evidence, R8)

After a real, committed flower-only completion for assignee B (operation `op`):

- **Other member**: in a raw transaction, create (if needed) A's flower account, lock it, read A's latest entry, insert an entry for A on `op` with chain-valid `seq`/`balance_after` (passes the 0006 chain trigger and `UNIQUE (operation_id, user_id, asset_type)`), then `SET CONSTRAINTS ALL IMMEDIATE` inside the DO wrapper: expect `UT007`.
- **Unrewarded asset**: in another raw transaction, create B's sunshine account and insert a chain-valid sunshine entry on `op`, then attempt a real COMMIT. Expected: the commit callback reports `false`; because `false` alone does not prove the outcome, the observer confirms directly that no sunshine entry for `op` and no sunshine account row from this transaction are committed.
- Afterwards the observer confirms that `op` still has exactly its original entry, the completion is unchanged, and both members' balances equal their pre-attack values.

## 10. Implementation batches and exact files (proposal; neither batch started)

### 10.1 M3-C1: backend batch

New:

- `services/api/src/services/TaskCompletionService.h/.cc` (D41).
- `services/api/tests/TaskCompletionUnitTests.h` (6.2, called from `UnitTest.cc`).
- `services/api/tests/TaskCompletionDbTests.h` (R1-R9, called from `DbGatedTest.cc` after block 14; small local helpers, reusing the existing probe/fixture utilities).

Modified:

- `services/api/src/services/TaskService.h/.cc` (D41: expose `taskFromRow(const drogon::orm::Row &, bool withHistory = false)` and the column list unchanged; no publish/list/get SQL or behaviour change).
- `services/api/src/controllers/TaskController.cc` (one POST route).
- `services/api/src/core/Errors.h/.cc` (four codes).
- `contracts/openapi.yaml` (0.5.0, additive).
- `services/api/tests/UnitTest.cc`, `DbGatedTest.cc`, `StaticCheckTest.cc`.
- Docs: this design's evidence section and short status notes.

Not modified: migrations, `init-roles.sql`, grants, `Db.h`/`Db.cc`, wallet policy/service/controller, `TaskPrivilegePolicy`, CMake (the services glob already includes new sources), deployment, dependencies.

### 10.2 M3-C2: frontend batch (after M3-C1 evidence)

Modified: `apps/web/src/api/tasks.ts`, `api/types.ts`, `api/error-messages.ts`, `stores/tasks.ts`, `views/TasksView.vue`, `tests/unit/tasks-view.spec.ts`. New: `apps/web/tests/unit/tasks-completion.spec.ts`. No dependency changes.

## 11. Execution, acceptance and rollout workflow (approved lighter workflow, 2026-10-14)

1. **Iteration**: Claude Code names the affected tests; the owner runs them.
2. **Milestone completion**: one relevant full regression per batch (M3-C1: all three CTest targets with `db_gated` not skipped; M3-C2: web tests and type check).
3. **Narrow repairs**: rerun only the affected checks; broaden only when shared behaviour changed or a concern remains unresolved.
4. **Command batches**: the owner still runs every terminal/build/test/Git/container/database action. Independent checks are grouped into fail-fast batches with one summary, instead of one reply per command. Deployment and database stages stay separately guarded batches, each starting only after explicit owner go-ahead.
5. **Rollout (owner only, guarded)**: no migration. Fresh main backup with a restore check -> preserve and tag the current M3-B API image **and** a copy of the served M3-B web `dist` with the same label -> deploy the new runtime (both startup policies `ok=true`) -> web build after the runtime is healthy.
6. **Compatibility check before relying on rollback** (on `usapp_test`, which will contain committed settlements): the preserved M3-B image starts and shows completed tasks and balances correctly (M3-B already reads `task_completions` and wallet balances; the check confirms it).
7. **Rollback (owner decision)**: restore the preserved API image together with the matching `dist`. The M3-B runtime has no completion route, so pending same-key retries cannot be served. Runtime rollback does not undo committed settlements; removing them needs a database restore, a separate owner decision that discards later writes.
8. **Evidence**: recorded once, in this document (a new Section 16 when results exist); other documents carry a one-line status and a link.

## 12. Manual acceptance (representative, on the real deployment)

- As the assignee, complete one flower-only and one both-reward task; the wallet card shows the new balances after reload; the partner sees only their own balances.
- The publisher cannot complete; an expired task cannot be completed.
- A double click mints once; an explicit retry after a forced network error mints once.
- One offline attempt is blocked and not replayed; one logout during an in-flight completion leaves the next account untouched.
- One no-cookie request and one wrong-Origin/CSRF request are rejected; responses carry `no-store`.
- Final database query: operations, entries, completions and `task.completed` audits match the actions; balances equal entry sums.

Input combinations already covered automatically are not repeated manually.

## 13. Approval status

| ID | Item | Status |
| --- | --- | --- |
| D30 | Lock order without a task-row lock (Option A) | Approved direction (owner statement recorded 2026-10-14); plan 9.8 rule 2 is refined for M3 one-off tasks; revisit in M6 |
| D32 | Closed-enum fault hook (8.1) | Approved direction (owner statement recorded 2026-10-14) |
| D33 | Persistent near-cap fixture | Deferred; not in either batch |
| D36 | Committed per-run settlement fixtures on literal `usapp_test` (Section 7) | Approved with M3-C1 (2026-10-14); tests create them only when the owner runs them |
| D37 | OpenAPI 0.5.0 completion operation (200 for first completion and replay, four codes) | Approved with M3-C1 (2026-10-14) |
| D38 | Single-statement decision + completion insert | Approved with M3-C1 (2026-10-14) |
| D39 | `TASK_ALREADY_COMPLETED` before `TASK_EXPIRED` for a new key | Approved with M3-C1 (2026-10-14) |
| D40 | Web completion feedback and refresh behaviour | Approved with M3-C2 (2026-10-14) |
| D41 | `TaskCompletionService` plus exposed `TaskService` mapper | Approved with M3-C1 (2026-10-14) |
| - | `db_gated` timeout | Keep 30 s; increase only after measurement with approval |
| - | New migration or grant change | None planned |

M3-C1 was approved on 2026-10-14 and is implemented (Section 16). M3-C2 with D40 was approved the same day and is implemented (Section 17).

## 14. Unverified assumptions

- `INSERT ... ON CONFLICT DO NOTHING` without a conflict target on `wallet_operations` (one constraint unique key plus one partial unique index) waits for an in-progress conflicting insert on either and then skips or inserts; the subsequent lookups in the same READ COMMITTED transaction see the committed winner.
- A `WITH n AS (SELECT clock_timestamp()) INSERT ... SELECT ... FROM n, tasks ... RETURNING` evaluates `clock_timestamp()` once and the 0007 BEFORE INSERT guard accepts the value (`completed_at <= clock_timestamp()` inside the trigger is later).
- Reading the latest entry in a statement issued after the `FOR UPDATE` statement sees a commit made by the previous lock holder (the chain trigger relies on the same READ COMMITTED behaviour).
- Drogon's binary binding of `int64_t` parameters with `::bigint` casts works on this path (plan 9.8 rule 6); M2 observed binding problems only with smallint/text mismatches.
- A deferred UT007/UW006 failure at COMMIT reaches the Drogon commit callback as `false` (hence 503 `RESULT_UNKNOWN`, not a confirmed failure).
- The lock/deadline/visibility cases fit within the 30 s `db_gated` timeout together with the existing blocks (M3-B run: 3.70 s).
- `pg_stat_activity` shows `wait_event_type = 'Lock'` for the waiting completion statement when observed by a separate `usapp_api` connection.
- The web wallet store can be reloaded from the tasks store without circular-import problems (as session/tasks already do).
- (Deferred with D33: the advisory-lock assumptions for a persistent cap fixture are not needed by the upcoming batches.)
- The preserved M3-B image reads committed M3-C settlements correctly (Section 11 item 6 makes this an explicit check).

## 15. Risks

- Option A depends on tasks staying immutable; M6 must revisit the lock order.
- Committed settlements in `usapp_test` and on main are permanent; mistakes on main need a backup restore.
- When D32 is implemented as approved, the seam will exist in the production binary (planned to be unreachable from HTTP, to be confirmed by a static check on the controller call).
- Real-service near-cap rejection (flower and sunshine) is deferred with D33; the cap check on the service path is covered only by code review, the pure pre-check tests and the database CHECK backstop, which do not prove that path.
- A completion decided just before the deadline may commit after it (approved D5c).
- RESULT_UNKNOWN during COMMIT cannot be produced on demand. No M3-C code or tests exist yet; the planned verification is code review plus a committed-success replay test (Section 6.3); no real lost-COMMIT test is planned or claimed.
- After an API rollback to M3-B, users with unconfirmed completions cannot retry them; they can only read the task status.
- The lighter workflow runs one full regression per milestone; a narrow repair reruns only affected checks, so an unexpected interaction elsewhere is caught at the next full regression rather than immediately.

## 16. M3-C1 implementation status (2026-10-14): implemented; owner-run automated tests passed (Section 16.5)

Claude Code wrote the code and tests below. Claude Code itself ran no build, test, package, container, database or Git command and created no fixture; the owner-run results are in Section 16.5. Statements below that tests were "not run" describe the state when written. M3-C1 is not accepted. M3-C2 (web) has not started.

### 16.1 Files

New: `services/api/src/services/TaskCompletionService.h/.cc`; `services/api/tests/TaskCompletionUnitTests.h`; `services/api/tests/TaskCompletionDbTests.h`.

Modified: `services/api/src/services/TaskService.h/.cc` (D41: the internal mapper is renamed `mapTaskRow` and exposed unchanged as `TaskService::taskFromRow(const drogon::orm::Row &, bool withHistory = false)`, plus `TaskService::taskColumnsSql()`; publish/list/get SQL and calls unchanged, history still passes `withHistory = true`); `services/api/src/controllers/TaskController.cc` (POST `/api/v1/tasks/{taskId}/completion` behind `AuthFilter`); `services/api/src/core/Errors.h/.cc` (`TASK_NOT_ASSIGNEE` 403, `TASK_ALREADY_COMPLETED` 409, `TASK_EXPIRED` 409, `WALLET_BALANCE_LIMIT` 409); `contracts/openapi.yaml` (0.5.0: `completeTask`, `CompleteTaskRequest`, `TaskSettlement`, `CompleteTaskResponse`, four codes; existing operations unchanged); `services/api/tests/UnitTest.cc`, `DbGatedTest.cc` (block 15 after the guard and block 14), `StaticCheckTest.cc`.

Not modified: migrations, `init-roles.sql`, grants, `Db.h`/`Db.cc`, wallet policy/service/controller, `TaskPrivilegePolicy`, CMake, frontend, deployment, dependencies, `.claude/`.

### 16.2 Implementation notes (as written; review these)

- Order exactly as Section 4.3: membership `FOR SHARE OF m` with the task read (no task-row lock, D30) -> claim `INSERT ... ON CONFLICT DO NOTHING RETURNING` with a C++-generated v4 operation id -> on no row, classification by `(actor, TASK_REWARD, key)` (same fingerprint: replay read and rollback; different: 409 key reused) and then by business source (409 already completed) -> per positive asset in `flower`, `sunshine` order: account `INSERT ... ON CONFLICT DO NOTHING` then `SELECT ... FOR UPDATE` -> latest-entry reads only after all locks -> cap pre-check -> D38 statement `WITH n AS (SELECT clock_timestamp() AS t) INSERT INTO task_completions ... RETURNING` (0 rows: 409 expired) -> one entry per positive asset with `int64_t` parameters and `::bigint` casts -> `task.completed` audit (`{"operation_id","flower","sunshine"}`) -> in-transaction read-back of facts -> `commitRequested` then commit.
- The fingerprint uses the database's canonical lowercase task id: `complete-task:v1:task_id=<len>:<id>;`.
- Request body must be empty/whitespace or exactly `{}`; non-uuid path ids are 404 `TASK_NOT_FOUND`.
- Replay and first success use the same facts query (`TaskService::taskColumnsSql()` over `task_completions` joined to `tasks`), so the bodies are identical; no balances are read for the response.
- Failure details are fixed labels `task_complete stage=<membership|claim|classify|replay|accounts|balances|decision|entry|fault_injection|audit|read_back|commit|transaction|input|pool> reason=<...>`; database exception objects and the transaction callback message are never read. Controller logs only these labels and the request id.
- D32: `SettlementFault` passed by value (default `kDefaultFault = None`); after the first entry, `FailAfterFirstEntry` runs the fixed probe bound to the service's own operation id. The controller call passes four arguments only.

### 16.3 Tests written (not run)

- Unit (`TaskCompletionUnitTests.h`): fingerprint; empty-body rule (11 cases); parameterized cap pre-check boundaries for both assets (explicitly not evidence for the real-service rejection path); outcome -> HTTP/code mapping; response shape without balance fields; `kDefaultFault`; D41 column-list regression.
- Static: contract 0.5.0 operations, schemas and codes with existing operations kept; the completion route line with `AuthFilter`; the controller's four-argument completion call and no `SettlementFault` reference; the header's enum and default; db_gated guard before block 15. The M3-B case's "no completion yet" assertions were replaced, and version checks moved to 0.5.0. The M3-A and M3-B migration checksum pins are unchanged.
- db_gated block 15 (`TaskCompletionDbTests.h`), permanent per-run fixtures under `@m3-fixture.test` after the literal guard:
  - R1+R9: three reward combinations; per task one task-sourced operation, one completion, expected entries, one `task.completed` audit with only the documented keys and no task text, one shared `xmin` across completion, operation, entries and audit; chain values 5 -> 7 flowers and 40 -> 50 sunshine; `WalletService` balances; callback sentinel; M3-B detail/open/history show the tasks as completed with the same `completed_at`.
  - R2+R4: same-key replay after an intervening settlement (byte-identical body, no new rows/audits, sentinel), key reuse on another task, other key on a completed task, partner key-value scope, publisher 403, other-space member 404, no-space 404, unknown task 404, invalid key 400 with no writes or new `task.completed` audits, fixed `membership/db_error` label without the echoed marker.
  - R3: concurrent different keys (one completion, one already-completed) and same key (one completion, one replay, identical bodies).
  - R5: a dedicated holder briefly holds B's flower-account lock so the faulted completion's backend PID is captured via `pg_blocking_pids`; after release, D32 failure (fixed label, sentinel), then `teardownEnded` for that PID, account lock release observed with `FOR UPDATE NOWAIT`, no committed rows or new audits, balances unchanged; same-key retry settles once with two entries.
  - R6: holder on a dedicated client holds the account lock; the waiting completion backend is identified by `pg_blocking_pids` containing the holder PID (not merely an unfinished request); the holder commits a non-task entry; the completion's entry is `seq 3`, `balance_after 15`.
  - R7: direct-insert seconds-precision deadlines (labelled API-unproducible rows); in both cases the completion is confirmed blocked by the holder PID via `pg_blocking_pids`: held past the deadline -> 409 expired, lock released, nothing committed; released before -> success with `completed_at < deadline_at`, and a same-key replay after the deadline returns the identical body.
  - R8: both raw attack transactions must capture their backend PID (otherwise the case fails); chain-valid other-member entry on R1's committed operation -> `UT007` at `SET CONSTRAINTS ALL IMMEDIATE`, then teardown confirmed; chain-valid unrewarded-asset entry -> real COMMIT reports `false`, teardown confirmed, and only then the observer checks that no row is committed and balances and the completion are unchanged.
- Not covered (explicit): real-service near-cap rejection (D33 deferred); forced lost COMMIT; HTTP-level 401/Origin/CSRF and `no-store` for the new route (owner live checks).

### 16.4 Static review findings and unverified points

- Nothing compiled. Drogon `int64_t` binding on this path, `Field::as<long long>` for `bigint`, and `HttpRequest::body()` returning a string view are assumed from Drogon v1.9.11 usage.
- `ON CONFLICT DO NOTHING` without a target over the source unique key and the partial key index, waiting behaviour under R3, and the volatile CTE in D38 being evaluated once are assumptions (Section 14) checked only when R1-R7 run.
- R7 adds roughly 5-6 s (two deadline windows); total `db_gated` time is unmeasured against the 30 s timeout.
- R8 depends on R1's committed operation and R2's partner completion (A's flower account); if R1 fails, R8 reports a failure rather than running.
- Review repair (2026-10-14): the R1 shared-`xmin` query now compares `audit_logs.target_id` with `$1::text` (the same parameter is `$1::uuid` for `task_completions`); lock waits are observed per holder PID with `pg_blocking_pids` instead of a database-wide query-text match. Assumed, not yet run: `pg_blocking_pids` and `pg_stat_activity` expose the waiting backend to the `usapp_api` observer, and `teardownEnded` on a pooled service connection reports `idle` after the faulted transaction is rolled back.

### 16.5 Owner-run automated verification (recorded 2026-10-14)

Run by the owner; Claude Code ran none of these commands.

- Backend test image build: succeeded.
- Full CTest: `unit` Passed 0.02 s; `db_gated` Passed 7.31 s, not skipped; `static_check` Passed 0.01 s; total 7.33 s; exit 0.
- `db_gated` stayed well inside the unchanged 30 s timeout; no timeout increase is needed.
- Because `db_gated` passed, R1-R9 ran on `usapp_test` and committed their per-run fixtures under `@m3-fixture.test` (growth as in Section 7). The assumptions listed in 16.4 that these cases exercise (Drogon parameter binding, untargeted `ON CONFLICT` waiting, the D38 statement, `pg_blocking_pids` observation and teardown) held in this run.

What this is and is not:

- It is automated verification of the M3-C1 backend on `usapp_test` only.
- It is **not** a formal acceptance decision (none is recorded), **not** a main-database deployment (the M3-C1 runtime has not been deployed), and **not** a live HTTP check: 401/Origin/CSRF/`no-store` on the completion route, end-to-end behaviour through Caddy and the web client are unverified.
- No Git commit or push of M3-C1 has been reported.
- Limits unchanged: no real-service near-cap rejection test (D33 deferred); no forced lost-COMMIT test; per-assertion counts were not supplied.
- M3-C2 (web, D40) is not approved and has not started.

## 17. M3-C2 implementation status (2026-10-14): implemented; owner-run automated web checks passed (Section 17.5)

Approved by the owner on 2026-10-14, including D40. Claude Code wrote the code and tests below; **no build, test, package, container or Git command was run, and the web `dist` was not built** (it is bind-mounted and served directly). M3-C2 is not accepted.

### 17.1 Files

- `apps/web/src/api/tasks.ts`: `completeTask(taskId, key)` -> `POST /api/v1/tasks/{taskId}/completion` through the shared `request()` (CSRF, Origin, same-origin cookies, offline block) with `Idempotency-Key` and no body.
- `apps/web/src/api/types.ts`: `TaskSettlement`, `CompleteTaskResponse`; four new codes in `ServerErrorCode`.
- `apps/web/src/api/error-messages.ts`: Chinese messages for `TASK_NOT_ASSIGNEE`, `TASK_ALREADY_COMPLETED`, `TASK_EXPIRED`, `WALLET_BALANCE_LIMIT` (the `RESULT_UNKNOWN` text now says "不会重复提交" so it fits both publish and completion).
- `apps/web/src/stores/tasks.ts`: completion actions and notices (below).
- `apps/web/src/views/TasksView.vue`: "完成" button, completion panel, notices.
- Tests: new `apps/web/tests/unit/tasks-completion.spec.ts`; `tasks-view.spec.ts` (obsolete "no completion control" assertion replaced, two view cases added, `completeTask`/`getWallet` mocked); `tasks-client.spec.ts` (one completion request/header case).

Not modified: backend, contract, migrations, grants, deployment, dependencies, `.claude/`, wallet store and API, session store.

### 17.2 Behaviour as implemented

- **Actions**: `completionActions[taskId] = { key, title, rewardFlowers, rewardSunshine, status, errorText }`, memory only. Statuses: `submitting`; `unknown` (the completion request may have reached the server, but its outcome cannot be confirmed: network/body-read error, unusable success response or any 5xx including `RESULT_UNKNOWN`); `not_sent` (offline block or local CSRF preflight failure: the completion request was not sent). A second click on a task with an action returns `busy` (one request). `unknown`/`not_sent` keep the key; only `retryCompletion(taskId)` (an explicit click) resends it. No automatic retry, no persistence, no offline queue.
- **Definitive answers**: a usable success response or a server 4xx ends the action and discards its key. Success facts are checked before the key is discarded: matching task id, completed status/time, operation id and bounded integer settlement amounts with at least one positive asset. `TASK_ALREADY_COMPLETED` and `TASK_EXPIRED` also reload open and history from page one. A server 4xx is shown as a rejected notice.
- **Confirmed success (first completion or replay)**: end the action; record a `completed` notice with the settlement amounts (feedback only); remove the task from `views.open` immediately; `loadFirst('open')` and `loadFirst('history')` (incrementing each view's sequence, so older first-page and load-more responses are ignored); `useWalletStore().load()` once. Settlement amounts are never written to wallet balances. Notices are separate from list and wallet state, so a reload failure leaves the confirmed notice in place.
- **Session guards**: every result checks the session generation and user id; after logout or account switch a late success or failure is discarded (no notice, no list reload, no wallet load). `reset()` clears actions, keys and notices.
- **View**: the "完成" button appears only for open tasks whose assignee is the current user and that have no pending action. A separate panel lists pending actions from their snapshots, with "重试完成" for `unknown`/`not_sent`, independent of list membership, plus dismissible notices. The view keeps no completion state of its own and does no work after an `await`, so account changes and unmount are covered by the store guards and reset.

### 17.3 Tests written (not run)

- `tasks-completion.spec.ts` (store, real `completeTask` through the shared client with stubbed `fetch`):
  - same key kept after 503 `RESULT_UNKNOWN`, 500, a network/body-read error, invalid JSON or an unusable settlement in HTTP 200; no notice or reload before confirmation; only an explicit retry resends it; a valid 200 replay is confirmed (notice, both reloads, one wallet load);
  - a duplicate click while submitting sends one request;
  - a definitive 409 `TASK_EXPIRED` ends the action and reloads both views, and the next attempt uses a new key;
  - a refresh that removes the task keeps the action, its key and the retry;
  - logout and account switch discard late success and failure (no notice, list reload or wallet load);
  - offline: blocked before fetch, not sent on `online`, an explicit retry reuses the key;
  - missing local CSRF token: preflight blocks the completion request, retains the key and produces no completion notice or reload; an explicit retry after token recovery reuses the key;
  - confirmed success with failing list and wallet reloads stays confirmed, the task leaves open, an older in-flight first-page response cannot re-insert it, and wallet balances are not set from the settlement.
- `tasks-view.spec.ts`: completion control only for the assignee's open task; the retry control survives a refresh that removes the task and resends the same arguments; a confirmed completion moves the task from the open section to the history section and loads the wallet once.
- `tasks-client.spec.ts`: completion POST path, `Idempotency-Key`, `X-CSRF-Token`, same-origin credentials, no body and no `Content-Type`.

### 17.4 Unverified points

- Nothing type-checked or run.
- Pinia unwrapping of the `completionActions` record ref, and the `vi.mock` partial module with the real `completeTask`, are assumed to behave as in existing specs.
- The live flow through Caddy and the real backend is unchecked: completion, wallet refresh, HTTP 401/403/409/503 handling.
- The web build and deployment are owner steps and have not been done.

### 17.5 Codex review repair (2026-10-14; verification pending)

At the owner's explicit request, Codex modified only `apps/web/src/stores/tasks.ts`, `apps/web/tests/unit/tasks-completion.spec.ts` and this Section 17. Completion preflight failures are separated from unconfirmed request outcomes; success fields are checked before deleting the action/key; confirmed success handling sits outside the request-error catch. The existing same-key retry table now includes body-read failure, invalid JSON and unusable settlement responses, with a local-CSRF preflight regression alongside the existing offline case. Static source review only: no tests, typecheck, build, Git, container or database commands were run; `dist` was not built. M3-C2 remains unaccepted.

### 17.5 Owner-run automated verification (recorded 2026-10-14)

Run by the owner; Claude Code ran none of these commands. Statements above that checks were "not run" describe the state when written.

- Web unit tests: 12 files, 105 tests passed, duration 20.56 s, exit 0. This run includes the Codex response-handling repairs made after Section 17.1-17.4 were written (those repairs are not itemized here).
- Type check: passed, exit 0.
- Together with Section 16.5, M3-C1 (backend CTest) and M3-C2 (web unit tests and type check) have each passed their automated checks.

When this entry was written, no formal acceptance, deployment, live verification or Git publication had been recorded. Later deployment and owner-reported live evidence follow in Section 18. D33 remains deferred.

## 18. Owner-run M3-C Rollout and Representative Live Checks (2026-10-14)

The owner ran the commands/checks; Codex and Claude Code did not. This entry
supersedes earlier delivery-time deployment-pending statements. Backend evidence
is in 16.5; web 12 files/105 tests passed in 20.56 s, exit 0, and typecheck exit 0
in 17.5. The owner subsequently requested the next feature and directly assigned
M4 to Codex (plan Section 10); this is not a separate formal M3-C acceptance.

- Fresh custom-format main backup: `backups/usapp_before_m3c_20261014_172137.dump`,
  204386 bytes. Single-transaction restore into `usapp_m3c_restore_20261014_172137`
  succeeded. Basic counts matched: migrations 8, users 2, spaces 1, members 2,
  audits 16, tasks 5; completions/accounts/operations/entries 0. Count comparison
  is not row-by-row verification; no agent inspected the backup.
- Preserved image `sha256:3563099da3340791a010711899f2ec8f5b27282811920855f217f352ab13dfda`,
  tag `usapp-api:before-m3c-20261014_172137`; served web copied to
  `backups/before-m3c-20261014_172137/web-dist` (19 files, index.html/sw.js present).
  Owner saved rollback-info.json. Neither runtime rollback nor preserved-image
  compatibility against committed settlements has been reported as exercised.
- API build exit 0, replacement healthy exit 0. Wallet policy: ok=true, 187 checks;
  task policy: ok=true, 146 checks; zero mismatch/missing/unexpected in both.
- Web build exit 0; 71 client modules; service worker precache 18 entries,
  164.83 KiB. Dist was bind-mounted and served after the backend was ready.
- Owner reported the representative guided live checks successful: flower-only
  settlement, duplicate-click protection and history; both-reward settlement
  (1 flower/3 sunshine), recipient control and publisher balance isolation;
  offline with explicit retry; a browser simulation that hid an actual successful
  response followed by same-key replay; a held late response after logout/account
  switch; no-cookie, CSRF, publisher, expiry and disallowed-Origin rejection plus
  no-store checks. Most results were brief success reports, not full per-request
  logs. The hidden-response simulation is not a real lost-COMMIT experiment.

Limits: targeted local-development evidence only; no forced lost COMMIT, no
real-service near-cap case (D33 deferred), no final main database/audit query or
separate formal acceptance reported, no post-M3-B Git commit/push evidence. No
current global balance/fixture-count claim is inferred. M4 source changes are
not included in these M3-C runtime/web results.
