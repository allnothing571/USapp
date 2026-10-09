# USapp Phase 1 M3 Design: One-Off Tasks with Single Completion and Reward Settlement

Status (updated 2026-10-09): **M3-A (database foundation) approved by the owner, implemented by Claude Code, and passed the recorded owner-run verification on `usapp_test` (Section 11.3); no separate formal acceptance decision is recorded, 0007 is not on the main database, and owner Git review/commit/push is next. M3-B and M3-C implementation are NOT approved.** The owner confirmed the product decisions listed in Section 2.3; all other items remain proposals. Original status: design only, written 2026-10-09 and revised the same day after two Codex reviews (revision 3), Asia/Shanghai. Every "proposal" or "recommendation" below is a suggestion, not an owner decision. Nothing in this document exists in the repository, and nothing has been built, tested, or executed.

Inputs read for this design: `CLAUDE.md`, `docs/claude-code-handoff.md`, `docs/phase1-plan.md` (Sections 4, 9.7-9.10, 9.13-9.20), `database/migrations/0006_wallet_ledger.sql`, `database/init-roles.sql`, `database/apply.ps1`, `contracts/openapi.yaml` (0.3.0), `services/api/src/core/Errors.*`, `services/api/src/db/Db.h`, `services/api/src/db/WalletPrivilegePolicy.*`, `services/api/src/services/WalletService.cc`, `ProfileService.cc`, `services/api/src/main.cc`, the `apps/web` stores, API client, and test file list. All reads were read-only.

Revision 2 changes (Codex review findings 1-9): the completion response no longer carries balances (Section 3.2, D31); the deadline decision point moves after all lock waits and is stored (Section 4.3, D5a/D5c); publish replay ignores the future-deadline rule (Section 4.2); a real partial-failure test uses an explicit fault-injection seam (Section 6.3, D32) and a separate real cap-rejection test uses a persistent near-cap fixture (D33); listing is paginated (D28); the `usapp_test` reset procedure is corrected and separated (Section 6.5, D29); zero/zero rewards become a product choice with a schema alternative (D22a); completion-to-settlement integrity is enforced in the database (Section 3.1, D34); frontend mutations are guarded across account changes (Section 5); the lock order is documented as a proposed refinement of plan Section 9.8 (D30); timezone claims are narrowed to Asia/Shanghai.

Revision 3 changes (second Codex review): a scoped ledger-insert check revalidates a task operation's complete entry set whenever any entry is added, including in later transactions (Section 3.1, D34); publish classifies an existing or concurrent claim before new-request deadline validation by inserting with `ON CONFLICT` first (Section 4.2); NULL-safe keyset pagination with exact cursor format and continuation predicates (Section 3.4); the deadline guarantee names the exact decision statement and no longer promises a short tail (Section 4.3, D5a/D5c); confirmed rollbacks are distinguished from unknown commit outcomes, which same-key retries reconcile (Section 4.4).

## 1. Scope

In M3:

- A member publishes a one-off task to the partner (the other member of the same space).
- Both members list the tasks of their space.
- The recipient completes the task once. Completion atomically creates (mints) the task's rewards in the recipient's wallet ledger (approved D1) and records the completion.

Excluded: counting tasks (M4), recurring schedules (M5), editing or cancelling tasks (M6), undo of a completion, redemption (M7-M9), notifications and the "notify partner" toggle, reminders, media, new admin endpoints, partner balances on the home card. No abstraction for those milestones is introduced: no period table, no schedule columns, no edit/version machinery.

## 2. Product decisions

### 2.1 Fixed requirements (already approved or established)

- D1: completion creates new flowers and sunshine for the recipient; the publisher is not debited. In ledger terms a `TASK_REWARD` operation has only credit entries.
- D2 (bounds only): balance at most 1,000,000 flowers and 1,000,000,000 sunshine; at most 999 flowers and 100,000 sunshine per ledger entry; operations exceeding a bound are rejected, never clamped.
- D21: the wallet card shows only the caller's own balances.
- C3/D3: once published, key task edits need recipient confirmation. M3 has no edit path at all, so nothing in M3 may change a published task's title, rewards, or deadline.
- Architecture and security baseline: session identity only; same-origin Origin + session-bound CSRF on every write; no-store responses; membership checked on the server; non-members get the same 404 as "not found"; no offline write queue; no post-commit side effects before a confirmed commit.
- V1 Sec 7 and 10: completion needs no proof; an unfinished task carries no penalty and simply disappears from the to-do list after its deadline.
- Timezone: the only supported configuration is `Asia/Shanghai` (contract `const`, every existing space). Asia/Shanghai has no daylight-saving gaps or overlaps, so local date/time to instant conversion is unambiguous there. M3 does **not** claim correct handling of arbitrary IANA zones; supporting another zone would need its own policy for nonexistent and repeated local times and is out of scope.
- Migration 0006 is applied on `usapp_test` and the main database and must never be edited; its grants and triggers must not be weakened.

### 2.2 Proposals that need owner decisions

Section 2.3 records which of the rows below the owner confirmed on 2026-10-09. The remaining rows stay proposals.

Existing IDs keep their numbers. D5-D10 and D20 are still proposals. New IDs start at D22.

| ID | Question | Proposal (not approved) |
| --- | --- | --- |
| D5 | Deadline semantics for one-off tasks | Optional. A local date plus an optional local time in Asia/Shanghai. Date only means "until the end of that local day". |
| D5a | Exact boundary and decision point | Completion succeeds when its decision time is strictly before `deadline_at`. The decision time is `clock_timestamp()`, read once by the decision statement (Section 4.3 step 7), which runs after the account locks (step 5) and the cap check (step 6). It is stored as `completed_at`. With a 23:00 deadline, decision time 22:59:59.999 succeeds and 23:00:00.000 fails. Date-only 2026-10-20 means `deadline_at` = 2026-10-21 00:00 local, exclusive. |
| D5b | Past deadline at publish | A **new** publish with a deadline not in the future is rejected with 400 `VALIDATION_ERROR`. A replay of an earlier successful publish, including one that was still uncommitted when the duplicate arrived, is returned even if its deadline has since passed (Section 4.2). |
| D5c | Delay between decision and commit | Accept decision-time semantics: steps 8-11 (completion insert, entries, audit, COMMIT) can still be delayed by an unbounded amount (lock waits, I/O, load). The stored decision is not re-evaluated, so a success may commit after the deadline. If any later step or the commit fails, the completion does not succeed. Alternative: re-check at COMMIT with a deferred trigger, which can reject a completion that was decided in time. |
| D6 | Undo of a completion | No undo in M3. |
| D7 | Publish to oneself | Not allowed; the assignee is always the partner, derived by the server. The request has no assignee field. |
| D8 | Task visibility | Both members see every task of their space, including the partner's completions and reward amounts. |
| D9 | "Notify partner" toggle | Not shown in M3. |
| D10 | Recipient reminders | Not in M3. |
| D20 | Duplicate-submission protection | Completion requires an `Idempotency-Key` header (Section 4.5). |
| D22 | Task reward ranges | Integer flowers 0-999 and sunshine 0-100,000 (the approved per-entry caps; one completion writes at most one entry per asset). |
| D22a | May both rewards be zero? | **Product recommendation:** no, at least one reward must be greater than 0, because V1 Sec 6 lists "flowers only, sunshine only, or both". This is a product choice, not a database necessity; Section 3.3 describes the alternative schema if zero/zero tasks are wanted. |
| D23 | Title and description limits | Title: trimmed, 1-40 Unicode code points, no control characters or line breaks (nickname rules). Description: optional, 0-500 code points after trimming; line feed allowed; other control characters rejected; CRLF normalized to LF. |
| D24 | Completion authority | Only the assignee, while still a member of the task's space. The publisher cannot complete their own published task. |
| D24a | Repeat completion | A retry with the same idempotency key returns the original result (200). A different key for an already completed task returns 409 `TASK_ALREADY_COMPLETED` and changes nothing. |
| D25 | Duplicate publish | Publish also requires an `Idempotency-Key`; a retry with the same key returns the already created task. |
| D26 | Grants for the new task tables | `usapp_api` SELECT and INSERT only on `tasks` and `task_completions`; no UPDATE/DELETE/TRUNCATE; re-applied by a new `init-roles.sql` section. Extend the startup privilege check to the task tables. |
| D27 | Behaviour when a reward would exceed a balance cap | Reject the whole completion with 409 `WALLET_BALANCE_LIMIT`; the task stays open; nothing is written. |
| D28 | Listing scope and pagination | Two paginated views (Section 3.2): `open` (every open task, no time window, keyset pagination) and `history` (completed or expired tasks whose completion time or expiry time is within the last 30 days, keyset pagination). Page size default 50, maximum 100. |
| D29 | Disposable test-database reset | A separate, explicit, owner-only procedure (Section 6.5). Not authorized by this design. |
| D30 | Completion lock order | Proposed M3 refinement of plan Section 9.8 (Section 4.3): no task-row lock; the business-source claim on `wallet_operations` serializes completions. |
| D31 | Completion response content | Immutable settlement facts only (task, operation id, credited amounts). No balances; the client reloads `GET /api/v1/wallet`. |
| D32 | Partial-failure test seam | A fault-injection hook on the real settlement path, reachable only from tests (Section 6.3). |
| D33 | Real cap-rejection fixture | A persistent, reusable near-cap fixture couple in `usapp_test`, created once through normal inserts (Section 6.3). |
| D34 | Completion-to-settlement integrity | Database triggers in 0007 bind each completion to exactly one matching `TASK_REWARD` operation and its exact entry set, revalidated on every relevant insert, including later transactions: deferred triggers on `task_completions`, on `wallet_operations` (scoped to `source_type = 'task'`), and on `wallet_ledger_entries` (acting only for task operations) (Section 3.1). |
| D35 | Unknown commit outcome | Return 503 `RESULT_UNKNOWN` when the commit callback reports failure; the client keeps the key and retries with it (Section 4.4). |

### 2.3 Owner decisions recorded on 2026-10-09

The owner confirmed these product decisions together with the M3-A approval. They do not authorize M3-B or M3-C code or contract changes.

- D22/D22a: integer rewards, flowers 0-999 and sunshine 0-100,000; at least one reward must be positive (Section 3.1 schema; the Section 3.3 alternative is not used).
- D27: exceeding a balance cap rejects the whole completion; no clamping; the task remains incomplete.
- D23: trimmed title of 1-40 Unicode code points without control characters or line breaks; optional description up to 500 code points, LF allowed, CRLF normalized by the service, other control characters rejected.
- D5/D5b: optional Asia/Shanghai local date and optional time; date-only expires at the next local midnight. New publishes require a future deadline (validated after claim classification, Section 4.2).
- D5a/D5c: the decision time is read after the account locks and cap checks and must be strictly before the deadline; later commit delay does not change that decision; success requires a confirmed commit.
- D6/D7/D8/D24: partner-only publication; both members see tasks, rewards and status; only the assignee completes; no undo in M3.
- D20/D24a/D25: publish and completion are idempotent; same-action retries return the original result; a different completion action for an already completed task is rejected without another reward.
- D31/D35: completion returns settlement facts, not current balances; the client reloads `GET /api/v1/wallet`. Unconfirmed outcomes keep the request key for an explicit retry, rather than claiming failure or retrying automatically.
- D28: paginated open tasks ordered by deadline with undated tasks last; recent history is 30 days by completion or expiry time, newest first; default page size 50, with load-more and refresh from page one.
- D26/D34: M3-A's narrowed task-table grants and settlement integrity constraints.

Still pending (engineering decisions and test arrangements outside M3-A): the exact contract shape and error-code list (M3-B), D29 test-database reset (not authorized), D30 lock order, D32 test seam, D33 persistent cap fixture (not authorized), the Section 3.4 cursor details, and any `db_gated` timeout change.

## 3. Schema and contract

### 3.1 Migration `0007_tasks_one_off.sql` (proposal; written only after approval)

0006 is not edited. One new numbered migration, applied by the owner with `apply.ps1`, following 0006's style (role preflight, explicit grants, triggers, no seed rows). The design below assumes D22a = "at least one positive reward"; Section 3.3 gives the variant for zero/zero tasks.

`tasks` (immutable after insert in M3):

| Column | Type | Rule |
| --- | --- | --- |
| `id` | uuid | primary key, default `gen_random_uuid()` |
| `couple_id` | uuid | NOT NULL |
| `creator_id` | uuid | NOT NULL; composite FK `(couple_id, creator_id)` to `couple_members (couple_id, user_id)` |
| `assignee_id` | uuid | NOT NULL; composite FK `(couple_id, assignee_id)` to `couple_members`; CHECK `assignee_id <> creator_id` |
| `title` | text | NOT NULL; CHECK `char_length(title) BETWEEN 1 AND 40` and `title = btrim(title)` |
| `description` | text | NOT NULL DEFAULT ''; CHECK `char_length(description) <= 500` |
| `reward_flowers` | integer | NOT NULL; CHECK `BETWEEN 0 AND 999` |
| `reward_sunshine` | integer | NOT NULL; CHECK `BETWEEN 0 AND 100000` |
| (table check) | | CHECK `reward_flowers > 0 OR reward_sunshine > 0` (product choice D22a) |
| `timezone` | text | NOT NULL; CHECK `timezone = 'Asia/Shanghai'` (supported configuration only) |
| `deadline_local_date` | date | NULL |
| `deadline_local_time` | time | NULL; CHECK time requires date |
| `deadline_at` | timestamptz | NULL; CHECK `(deadline_at IS NULL) = (deadline_local_date IS NULL)` |
| `create_idempotency_key` | text | NOT NULL; CHECK length 16-128 |
| `create_fingerprint` | text | NOT NULL; CHECK `~ '^[0-9a-f]{64}$'` |
| `created_at` | timestamptz | NOT NULL DEFAULT now() |

Unique: `(creator_id, create_idempotency_key)` and `(id, couple_id)` (for composite FKs). Indexes for the paginated views are in Section 3.2.

`deadline_at` is computed in SQL at insert: `(date + time) AT TIME ZONE 'Asia/Shanghai'`, or `(date + 1) AT TIME ZONE 'Asia/Shanghai'` for date-only. The client never sends an instant.

`task_completions` (append-only):

| Column | Type | Rule |
| --- | --- | --- |
| `task_id` | uuid | primary key (one completion per task); composite FK `(task_id, couple_id)` to `tasks (id, couple_id)` |
| `couple_id` | uuid | NOT NULL |
| `completed_by` | uuid | NOT NULL; composite FK `(couple_id, completed_by)` to `couple_members` |
| `operation_id` | uuid | NOT NULL; **UNIQUE** (no operation reused across completions); composite FK `(operation_id, couple_id)` to `wallet_operations (id, couple_id)` |
| `reward_flowers`, `reward_sunshine` | integer | NOT NULL; snapshot of what was credited |
| `completed_at` | timestamptz | NOT NULL; the decision time (D5a), supplied by the service |

Status is derived, not stored: completed if a `task_completions` row exists; otherwise expired if `deadline_at <= now()`; otherwise open. No scheduler job is needed.

Completion-to-settlement integrity (D34). Database invariants, enforced by triggers in 0007:

1. `task_completions` BEFORE INSERT (row): the task exists in the same couple; `completed_by = tasks.assignee_id`; the snapshot amounts equal the task's rewards; `completed_at <= clock_timestamp()` (no future timestamps); if `deadline_at` is set, `completed_at < deadline_at`. The referenced operation must have `operation_type = 'TASK_REWARD'`, `source_type = 'task'`, `source_id = task_id::text`, `actor_user_id = completed_by`, and the same `couple_id`. Violations raise custom SQLSTATEs (proposal `UT002`-`UT004`).
2. One validation function, `usapp_task_settlement_valid(operation_id)`, defines a valid settlement: the operation is `TASK_REWARD` with `source_type = 'task'`; exactly one `task_completions` row references it; its ledger entries are exactly one entry per positive-reward asset, each with `user_id = completed_by` and `amount` equal to the snapshot; and it has no other entries. On failure it raises `UT005`. Three deferred constraint triggers (checked at commit or at `SET CONSTRAINTS ... IMMEDIATE`) call it:
   - on `task_completions` AFTER INSERT;
   - on `wallet_operations` AFTER INSERT, scoped `WHEN (NEW.source_type = 'task')`: a task-sourced operation must be completed and valid by commit time;
   - on `wallet_ledger_entries` AFTER INSERT, for every new entry. The row function looks up the entry's operation and returns at once unless its `source_type` is `'task'`; for task operations it revalidates the complete entry set. A `WHEN` clause cannot reference another table, so the scoping happens inside the function.

   The ledger trigger is what protects a settlement **after** its original commit. 0006's `UNIQUE (operation_id, user_id, asset_type)` still allows a later transaction to append an entry for the other member, or for an asset the task did not reward, to an already committed task operation. Such an insert passes 0006's chain trigger (it is valid arithmetic for that account), but at commit the ledger trigger finds an entry set that no longer matches the completion snapshot and rejects the whole transaction. The original settlement and every balance stay as they were. A second entry for the same rewarded asset and recipient is already rejected by the 0006 unique key.

   The checks are deferred because the initial settlement inserts the completion (step 8) before its entries (step 9); immediate checks would fail mid-transaction. The added triggers only add checks to 0006 tables: 0006 is not edited, and its grants, append-only triggers, chain trigger, single-row-insert trigger, and "operation has entries" check are unchanged. Non-task operations (M2-A fixtures use `wallet_test` and `psql_test`) cost one primary-key lookup per entry and are otherwise unaffected.
3. `UNIQUE (operation_id)` on `task_completions` prevents one operation from settling two completions; the 0006 business-source uniqueness `(operation_type, source_type, source_id)` prevents two operations for one task.

Service-only checks (no database equivalent): caller authentication and membership lookup, mapping outcomes to HTTP codes, idempotency classification (replay / reused key / already completed), the balance-cap pre-check (the 0006 balance CHECK is its backstop), and choosing the decision time.

Append-only triggers (0006 pattern): statement-level `BEFORE UPDATE OR DELETE OR TRUNCATE` on both new tables raising `UT001`.

Grants (D26): `REVOKE ALL ... FROM PUBLIC, usapp_api, usapp_push`; `GRANT SELECT, INSERT ON tasks, task_completions TO usapp_api`. No lock column is needed because no task table is row-locked (Section 4.3). A new `init-roles.sql` section 8, after the ALL TABLES grant, re-applies this per existing table, as section 7 does for wallet tables. Section 7 is not changed.

Same-couple membership: every user reference is a composite FK into `couple_members (couple_id, user_id)` using the unique constraint 0006 added. Task, completion, and operation share one `couple_id` through composite FKs. Items 1-3 above add the semantic links (type, source, recipient, amounts, complete entry set) that composite FKs cannot express.

Snapshots: the task row is immutable in M3. The completion row stores the credited amounts and the decision time. No balance is stored in the completion (D31).

### 3.2 Contract (OpenAPI 0.4.0, additive)

All operations are behind `AuthFilter`; pre-authentication sessions get 401; a user without a space gets 404 `SPACE_NOT_FOUND`; all responses are no-store.

| Operation | Request | Success |
| --- | --- | --- |
| `POST /api/v1/tasks` (`createTask`) | Header `Idempotency-Key`; body exactly `{title, description?, reward_flowers, reward_sunshine, deadline?: {local_date, local_time?}}` (unknown fields 400) | 201 `{ "task": Task }`; same-key replay 200 with the same task |
| `GET /api/v1/tasks?view=open\|history&cursor=&limit=` (`listTasks`) | `view` required; `limit` 1-100 (default 50); opaque `cursor` | 200 `{ "tasks": [Task], "next_cursor": string \| null }` |
| `GET /api/v1/tasks/{taskId}` (`getTask`) | none | 200 `{ "task": Task }` |
| `POST /api/v1/tasks/{taskId}/completion` (`completeTask`) | Header `Idempotency-Key`; empty body | 200 `{ "task": Task, "settlement": { "operation_id": uuid, "flower": int, "sunshine": int } }`; same-key replay 200 with the identical body |

`Task`: `id`, `title`, `description`, `creator_id`, `assignee_id`, `reward_flowers`, `reward_sunshine`, `deadline` (`{local_date, local_time|null, at}` or null), `status` (`open|completed|expired`), `completed_at` (or null), `created_at`.

Completion response (D31): only immutable facts. `settlement.flower` and `settlement.sunshine` are the credited amounts (0 for a non-rewarded asset); `operation_id` identifies the ledger operation. In M3 a completed task can never change, so a replay rebuilds exactly the same body from the `task_completions` row and the task. **No balances are returned.** After a successful completion the client reloads its own balances with `GET /api/v1/wallet`. A replay must never be used to set current balances.

Alternative considered for D31: store each asset's balance after the operation in `task_completions` (for a non-rewarded asset, the balance read under the account lock), and return them as a labelled historical snapshot. Rejected as the default: it adds columns and still requires the client to reload the current balance, because later operations change it.

Listing views (D28):

- `open`: tasks without a completion and with `deadline_at IS NULL OR deadline_at > now()`. Order `(deadline_at ASC NULLS LAST, created_at ASC, id ASC)`. No time window: every open task is reachable through pages.
- `history`: completed tasks with `completed_at > now() - 30 days`, and expired tasks (no completion) with `deadline_at <= now()` and `deadline_at > now() - 30 days`. Sort key `history_at` = `completed_at` for completed, `deadline_at` for expired. Order `(history_at DESC, id DESC)`. The window uses completion time or expiry time, **not** creation time: a task created 40 days ago and completed today appears.
- Keyset pagination on those keys, with the exact cursor format and NULL-safe continuation predicates in Section 3.4. Status depends on `now()`, so between page requests a task can move from `open` to `history` (or appear in `history`): a client may then see it in neither the page already loaded nor the next one until it reloads. This is documented, not hidden; the UI's "reload" restarts from the first page.
- Indexes (proposal): `tasks (couple_id, deadline_at, created_at, id)` and `task_completions (couple_id, completed_at DESC, task_id)`.

New error codes (added to `ErrorEnvelope.code`, `core/Errors.*`, and web `error-messages.ts`):

| Code | HTTP | When |
| --- | --- | --- |
| `TASK_NOT_FOUND` | 404 | Task missing or in another space (indistinguishable) |
| `TASK_NOT_ASSIGNEE` | 403 | Caller is a member but not the assignee |
| `TASK_ALREADY_COMPLETED` | 409 | Completed by an earlier request with a different key |
| `TASK_EXPIRED` | 409 | Decision time not before the deadline (D5a) |
| `PARTNER_REQUIRED` | 409 | Publishing while the space has only one member |
| `WALLET_BALANCE_LIMIT` | 409 | Crediting would exceed a D2 balance cap |
| `IDEMPOTENCY_KEY_REUSED` | 409 | Same key, same scope, different request fingerprint |
| `RESULT_UNKNOWN` | 503 | Publish or completion: the commit outcome could not be confirmed; retry with the same key (Section 4.4, D35) |

A missing or malformed `Idempotency-Key` or an invalid cursor returns 400 `VALIDATION_ERROR`. No existing operation or schema changes.

### 3.3 Alternative if zero/zero tasks are allowed (D22a)

The ledger requires entries only for operations that exist; it does not forbid tasks without rewards. If the owner wants zero/zero tasks:

- Drop the table check `reward_flowers > 0 OR reward_sunshine > 0`.
- `task_completions.operation_id` becomes nullable, with CHECK `(operation_id IS NULL) = (reward_flowers = 0 AND reward_sunshine = 0)`.
- The completion claim and idempotency record move from `wallet_operations` to `task_completions`: add `idempotency_key` and `request_fingerprint` columns with UNIQUE `(completed_by, idempotency_key)`, and claim with `INSERT INTO task_completions ... ON CONFLICT DO NOTHING RETURNING` (task-id primary key and key uniqueness both act as conflict targets).
- Rewarded completions additionally insert the wallet operation, with the same key and fingerprint so the 0006 scope still holds.
- The item 2 validation triggers apply unchanged to rewarded tasks; zero/zero completions write no wallet rows and no wallet audit detail.

Tradeoff: one more nullable link and two idempotency stores to keep consistent, versus the simpler single claim on `wallet_operations`.

### 3.4 Cursor format and continuation predicates (D28)

Cursor representation: base64url (no padding) of a compact JSON object with exactly these fields.

- `open` view: `{"v":1,"view":"open","dl":<deadline_at or null>,"ca":<created_at>,"id":<uuid>}`
- `history` view: `{"v":1,"view":"history","ha":<history_at>,"id":<uuid>}`

Timestamps are UTC strings with exactly six fractional digits (`YYYY-MM-DDTHH:MM:SS.ffffffZ`), produced by the server from the last row of the page. PostgreSQL `timestamptz` has microsecond resolution, so these strings round-trip exactly through `$n::timestamptz`. The uuid is lowercase canonical form; uuid ordering in PostgreSQL is bytewise, which the `ORDER BY ... id` uses as well.

Every query first restricts rows to the caller's couple, taken from the session's membership and never from the cursor, and applies the view's status filter (Section 3.2). A cursor only positions within that set; a forged or foreign cursor cannot reveal another couple's tasks. Cursors are not signed and carry no secrets.

`open` view, order `deadline_at ASC NULLS LAST, created_at ASC, id ASC`. With the last row's values `L_dl`, `L_ca`, `L_id`:

- Last row had a deadline (`L_dl` not null): the next page is every row that either has a deadline and sorts after the last row, or has no deadline (this is the transition to undated tasks):
  `(t.deadline_at IS NOT NULL AND (t.deadline_at, t.created_at, t.id) > (L_dl, L_ca, L_id)) OR t.deadline_at IS NULL`
- Last row had no deadline (`L_dl` null): continue only within undated tasks:
  `t.deadline_at IS NULL AND (t.created_at, t.id) > (L_ca, L_id)`
- First page (no cursor): no predicate.

The service selects one of these two predicate forms from the cursor (two SQL texts, both with bound parameters), so NULL never enters a row comparison. Equal deadlines are ordered by `created_at`, and equal `created_at` values by `id`, which is unique, so the order is total and no row is skipped or repeated while the underlying set is stable.

`history` view, order `history_at DESC, id DESC`, where `history_at = COALESCE(completed_at, deadline_at)` is never null within the view: `(history_at, t.id) < (L_ha, L_id)`.

Every query fetches `limit + 1` rows; the extra row only signals that `next_cursor` is non-null and is not returned.

Malformed cursors return 400 `VALIDATION_ERROR` and run no query: not base64url, not JSON, unknown `v`, a `view` that differs from the request's `view` parameter, missing or extra fields (including a field that belongs to the other view), a timestamp not in the exact format, a non-canonical uuid, or a null `ca`, `ha`, or `id` (only `dl` may be null).

## 4. Settlement and idempotency

### 4.1 Lifetime and exactly-once (all task transactions)

Use the M1 `ProfileService` pattern: a heap context owns its `TxScope`; the error handler stored in `TxScope::onError_` captures the context **weakly**; SQL and commit callbacks capture it strongly; every end path goes through one `complete()` guarded by a `completed` flag; success is reported only from the commit callback. The context's `scope` is never reset while a `TxScope` method runs. `Db.h`/`Db.cc` are not changed. A db_gated sentinel test (like M1 block 10) proves the request callback is released after success, rejection, and SQL failure.

### 4.2 Publish transaction and replay ordering

Revision 2 looked up the key, then validated the deadline, then inserted. That order was wrong: while the original successful publish is still uncommitted, a duplicate's lookup sees no row, and its deadline validation could reject it before it ever waited on the uniqueness claim. Revision 3 claims first and validates the deadline only on the newly inserted branch:

1. Validate the body shape and the non-temporal rules (D22, D23) and compute the fingerprint. Failure: 400 `VALIDATION_ERROR`. No deadline rule here.
2. `SELECT` the caller's membership and the partner's membership in one statement, `FOR SHARE OF` the caller's `couple_members` row (the role keeps DML on `couple_members`, so the lock is permitted). No row: 404 `SPACE_NOT_FOUND`. Only one member: 409 `PARTNER_REQUIRED`.
3. **Claim**: `INSERT INTO tasks (..., deadline_at = <computed in SQL>) ON CONFLICT (creator_id, create_idempotency_key) DO NOTHING RETURNING id, (deadline_at IS NULL OR deadline_at > clock_timestamp()) AS deadline_in_future`. If another transaction holds an uncommitted row with the same key, this statement waits until that transaction ends. If it committed, the statement inserts nothing; if it rolled back, this statement inserts the candidate normally.
4. No row returned (an existing committed claim): read the row for `(creator_id, create_idempotency_key)`.
   - Same fingerprint: replay. Roll back; 200 with the stored task. **No deadline rule is applied**, so the replay succeeds even after its deadline has passed, whether the original had already committed or committed while this request waited.
   - Different fingerprint: roll back; 409 `IDEMPOTENCY_KEY_REUSED`.
   - No row found (not expected, because `ON CONFLICT DO NOTHING` skips only after a committed conflict): roll back; 500 `INTERNAL_ERROR`.
5. Row returned (this request's new candidate): if `deadline_in_future` is false, roll back; 400 `VALIDATION_ERROR` (D5b). The candidate never commits.
6. Insert `audit_logs` (`task.created`, actor user, couple, target task; detail without title or description text).
7. Commit; respond 201 only from the commit callback.

Assumptions this ordering relies on (to be verified at implementation): `tasks` has no temporal CHECK and no BEFORE INSERT trigger, so the candidate insert cannot fail for a temporal reason before conflict classification; the non-temporal CHECKs mirror step 1, so a well-formed duplicate cannot fail them (if they ever diverge, the statement errors and the request ends with 500, never with a wrong replay); `deadline_in_future` is computed in the same statement as the insert, from the stored `deadline_at`.

Response codes: 201 new task; 200 replay; 400 invalid body or (new requests only) a deadline not in the future; 404 no space; 409 `PARTNER_REQUIRED` or `IDEMPOTENCY_KEY_REUSED`; 500 internal error. Section 4.4 covers an unknown commit outcome.

### 4.3 Completion transaction (exact order)

Times are distinguished as follows:

- **Admission time**: when the request reaches the service. Not used for the decision.
- **Decision time**: the value of `clock_timestamp()` read once by the decision statement in step 7. Step 7 runs after the claim (step 3, which can wait), the account locks (step 5, which can wait), and the cap check (step 6). It decides expiry and is stored as `completed_at`.
- **Commit time**: when COMMIT finishes, after steps 8-11. No bound is promised on how long after the decision time this is (Section 4.3 tail and D5c).

No task row is locked. Drogon v1.9.11 reports SQL errors without SQLSTATE, so the design never depends on catching a unique violation: claims use `INSERT ... ON CONFLICT DO NOTHING RETURNING`.

1. Begin `TxScope` (READ COMMITTED).
2. Read the task joined with the caller's membership, `FOR SHARE OF` the caller's `couple_members` row. No row: 404 `TASK_NOT_FOUND`. Caller not the assignee: 403 `TASK_NOT_ASSIGNEE`. Compute the fingerprint (Section 4.5). No deadline check here.
3. Claim: `INSERT INTO wallet_operations (id, couple_id, actor_user_id, operation_type, source_type, source_id, idempotency_key, request_fingerprint) VALUES (..., 'TASK_REWARD', 'task', task_id, key, fingerprint) ON CONFLICT DO NOTHING RETURNING id`, with the operation id generated in C++. This may wait for a concurrent claimer and then sees its committed result.
4. No row returned: look up `(actor_user_id, 'TASK_REWARD', key)`.
   - Same fingerprint: replay, built from the `task_completions` row and the task; roll back; 200. **No deadline check**: the original decision stands.
   - Different fingerprint: roll back; 409 `IDEMPOTENCY_KEY_REUSED`.
   - Not found: the task's source is held by another key; roll back; 409 `TASK_ALREADY_COMPLETED`.
5. For each positive-reward asset, in ascending `(user_id, asset_type)` order: `INSERT INTO wallet_accounts ... ON CONFLICT DO NOTHING`, then `SELECT ... FOR UPDATE` (existing `lock_token` grant). This may wait. Zero-reward assets are skipped (no account touch, no entry).
6. Read each locked account's latest entry. If `balance + reward` exceeds the cap: roll back; 409 `WALLET_BALANCE_LIMIT` (D27).
7. **Decision**: one statement reads the clock once and compares it, for example `WITH n AS (SELECT clock_timestamp() AS t) SELECT n.t AS decided_at, (t.deadline_at IS NULL OR n.t < t.deadline_at) AS still_open FROM n, tasks t WHERE t.id = $1`. Not open: roll back; 409 `TASK_EXPIRED`.
8. `INSERT INTO task_completions (..., completed_at = decided_at)`. The Section 3.1 item 1 trigger re-verifies assignee, amounts, operation type/source/actor, and `completed_at < deadline_at`.
9. Insert one ledger row per statement: flower, then sunshine (0006 rejects multi-row inserts with `UW005`). The chain trigger re-verifies `seq` and arithmetic.
10. Insert `audit_logs` (`task.completed`, actor = recipient, couple, target task, detail `{operation_id, flower, sunshine}`).
11. Commit. Deferred checks run: 0006 "operation has entries", and the Section 3.1 item 2 settlement validation through its `task_completions`, `wallet_operations`, and `wallet_ledger_entries` triggers. Respond 200 only from a confirmed commit callback, with values computed in the transaction.

Delay after the decision: steps 8-11 can still take an unbounded time. FK checks take key-share locks (no M3 or earlier flow is expected to hold conflicting locks, but this is not guaranteed), and inserts and COMMIT depend on I/O and server load. Such a delay does not change `completed_at` or the decision already made. If any of steps 8-11 or the COMMIT fails, the completion does not succeed (Section 4.4). Under D5c, a completion decided before the deadline may therefore commit after it.

Relation to plan Section 9.8 (D30, proposed refinement, **not** approved): Section 9.8 binds "business row -> `couple_members` (FOR SHARE) -> operation insert -> accounts sorted". M3 instead uses "`couple_members` (FOR SHARE) -> operation claim -> accounts sorted -> decision". The orders are not identical. Reasons: the M3 task row is immutable, so there is no business-row state to protect; row-locking `tasks` would require an UPDATE privilege (a lock column) on a table that otherwise needs none; and the 0006 business-source unique key already serializes completions of one task. When M6 makes tasks editable, a business-row lock will be needed and this refinement must be revisited.

### 4.4 Failure handling and unknown commit outcomes

The server still reports success only after a confirmed commit (the commit callback reports `true`). Failures fall into two classes:

- **Confirmed not committed**: a statement fails before COMMIT is sent (SQL error, cap rejection, expiry, an immediate trigger, a business 4xx decision). The service never sends COMMIT; Drogon's error wrapper requests a rollback, and if that request were lost the server would still abort the transaction when the connection ends. Nothing of this request can commit. Business outcomes are returned as their 4xx codes; SQL errors as 500.
- **Commit outcome unknown**: the commit callback reports `false`. Drogon v1.9.11 gives only a boolean here, so the service cannot tell a commit rejected by the server (for example a deferred check) from a connection lost after the server committed. The service must not report success and must not claim that nothing was committed. It returns 503 with a new error code `RESULT_UNKNOWN` (proposal) and logs the operation id.

Reconciliation by same-key retry: the client keeps the action's idempotency key after any 5xx, network error, or timeout, and retries with the **same** key. If the original committed, the retry's claim conflicts and returns the replay (200, identical body). If it did not commit, the retry settles normally. Business-source uniqueness and the key scope make a duplicate settlement impossible either way. A deterministic deferred-check failure fails again on retry. The same reconciliation applies to publish.

Not reproducible in automated tests: forcing a connection loss exactly during COMMIT. Section 6.3 covers both reconciliation outcomes instead: a committed success followed by a same-key retry (models "committed, response lost") and a confirmed rollback followed by a same-key retry.

### 4.5 Idempotency definitions

- Key: client-generated random UUID (36 chars, within 0006's 16-128 rule), sent as `Idempotency-Key`. One key per user action; reused only for retries of that same action; kept in memory only, never persisted or queued.
- Completion scope: `(actor_user_id = recipient, 'TASK_REWARD', key)`, the existing 0006 unique index. Publish scope: `(creator_id, key)` on `tasks`.
- Request fingerprint: lowercase SHA-256 hex of a canonical string. Completion: `complete-task:v1:<task_id>`. Publish: `create-task:v1:` plus the normalized title, description, rewards, and deadline in fixed order with explicit lengths.
- Same key, same fingerprint: replay; nothing written.
- Same key, different fingerprint: 409 `IDEMPOTENCY_KEY_REUSED`; nothing written.
- Different key, task already completed: 409 `TASK_ALREADY_COMPLETED`; nothing written. Business-source uniqueness guarantees one mint per task regardless of keys, tabs, or devices.
- Concurrent same-key requests: the second waits on the unique index, then replays.

## 5. Frontend

- New `TasksView` route `/tasks`, linked from the home page; within the existing guard (authenticated and in a space).
- New `src/api/tasks.ts` and memory-only `src/stores/tasks.ts`, registered in `resetBusinessState()` and reset on user change, like the wallet store.
- Two sections with "load more": the open view and the history view (D28). The open view further splits into "to complete" (assignee = me) and "published by me". States: loading, ready, empty, error with retry; no stale rows next to an error.
- Publish form: client-side mirror of the D22/D23 rules (server authoritative); one key per submission attempt, reused for a retry of that submission and discarded after a definitive response.
- Complete button: disabled while in flight; one key per click, reused for a retry of that click.
- Mutation lifetime guard (for publish and completion, successes **and** failures): before sending, capture `ownerUserId = session.user.id` and the tasks store's `generation`. When the response settles, apply it only if `session.user.id === ownerUserId` and the generation is unchanged; otherwise discard it silently: no list change, no error banner, no wallet reload. `reset()` (logout, user change, failed bootstrap) increments the generation and clears in-memory idempotency keys.
- After an applied completion success: mark the task completed from the response, then call the wallet store's `load()` (`GET /api/v1/wallet`). The response's settlement amounts are shown as "+N" feedback only and are never written into wallet balances. A replay response is handled the same way.
- On 409 `TASK_ALREADY_COMPLETED` or `TASK_EXPIRED` (applied): reload the open view.
- Key lifetime by outcome: a definitive response (2xx, or a 4xx business or validation code) ends the action and discards its key. A 5xx (including 503 `RESULT_UNKNOWN`), a network error, or a timeout leaves the outcome unknown: the UI says the result is not confirmed and offers retry, which reuses the **same** key, so a committed original replays and an uncommitted one settles once. Nothing is retried automatically, and the key is still discarded on reset (logout or account change), since the mutation guard would drop the response anyway.
- Offline: writes are blocked by the existing client guard and never queued; nothing is sent automatically after reconnecting.
- New Chinese messages for the new error codes.

## 6. Tests and fixtures

### 6.1 Unit and static

- Unit: title/description/reward/deadline validation; fingerprint canonicalization; response shaping (no balance fields in the completion body); deadline arithmetic for date-only and date+time in Asia/Shanghai; cursor encode/decode and rejection of malformed cursors.
- Static: contract operations, schemas, error codes, and the absence of balance fields in `completeTask`; routes registered with `AuthFilter` and the correct methods; controller in the CMake list; 0007 grants, triggers (including the `source_type = 'task'` scope of the `wallet_operations` trigger and the task-only early return of the `wallet_ledger_entries` trigger), and no UPDATE/DELETE grants; `init-roles.sql` section order; 0006 unchanged (its recorded checksum `2633e17e...7d543d`).

### 6.2 Rollback-only database tests (db_gated, `usapp_test`, as `usapp_api`)

Independent transaction per expected failure (M2-A pattern; SQLSTATE exposed through the DO wrapper):

- 0007 constraints, composite FKs, append-only triggers, task privilege matrix.
- Adversarial same-couple integrity cases (D34), each expected to fail: completion referencing a `REWARD_REDEMPTION` operation; an operation whose `source_id` is another task of the same couple; an operation whose actor is the publisher; a completion with `completed_by` = publisher; an entry crediting the publisher instead of the assignee (fails at `SET CONSTRAINTS ALL IMMEDIATE`); an entry amount different from the snapshot; an extra entry for a non-rewarded asset; reusing one operation for a second completion (unique violation); a `source_type = 'task'` operation with no completion (fails at `SET CONSTRAINTS ALL IMMEDIATE`); within the settling transaction itself, an additional entry for the other member or an unrewarded asset (fails at `SET CONSTRAINTS ALL IMMEDIATE`); `completed_at` at or after `deadline_at`; `completed_at` in the future.
- Non-task operations are unaffected: an M2-A-style operation with a test `source_type` and an entry for any member still passes `SET CONSTRAINTS ALL IMMEDIATE`.

### 6.3 Real-service tests with committed fixtures

All use the real service path and commit to `usapp_test` only (Section 6.4).

- Concurrent completion, different keys: exactly one 200, the other 409 `TASK_ALREADY_COMPLETED`; one operation, one completion, the expected entries.
- Concurrent completion, same key: both 200 with identical bodies; one operation.
- Retries and replay content:
  - single-asset rewards: a sunshine-only task and a flower-only task each produce one entry; `settlement` shows 0 for the other asset; replay bodies are identical;
  - intervening settlement: complete task 1 (key K1), then complete task 2 for the same recipient, then replay K1: the body is byte-for-byte the original and contains no balances; `GET /api/v1/wallet` (service level) shows the current total;
  - same key on another task: 409 `IDEMPOTENCY_KEY_REUSED`; the partner reusing the key value for their own action is a different scope.
- Unauthorized and non-member access: publisher completing (403); member of another space (404); user without a space (404); pre-authentication 401 at controller level (static/unit).
- Deadline decision across a wait: a committed task with `deadline_at` about 2 s in the future (inserted directly, as publish rejects past deadlines only for new requests); a raw transaction holds `FOR UPDATE` on the recipient's account; the completion starts before the deadline and waits. Case A: release the lock after the deadline: 409 `TASK_EXPIRED`, nothing written. Case B: release before the deadline: 200 with `completed_at < deadline_at`. A replay of case B's key after the deadline still returns 200 with the identical body.
- Publish ordering (Section 4.2). The "held publisher" is a raw test transaction that inserts the `tasks` row with key K, the service's own canonical fingerprint (computed by the same pure function the service uses), and a deadline about 1 s ahead, and keeps it uncommitted:
  - held, then committed: wait until the deadline has passed; start the real publish with key K and the identical body; it must not finish within a bounded wait (about 300 ms), and the observer sees a `Lock` wait; commit the raw transaction; the publish returns 200 with the stored task (a replay, not 400);
  - held, then rolled back: same start; roll the raw transaction back; the publish proceeds as a new request, finds the deadline passed, and returns 400; no task with key K is committed;
  - genuinely new expired publish (no prior claim): 400, nothing committed;
  - committed publish replayed after its deadline: 200 with the stored task;
  - same key with a different body: 409 `IDEMPOTENCY_KEY_REUSED`.
- **Partial-failure rollback on the real path (D32):** the settlement implementation accepts an optional test-only fault hook (an argument of the service method that the controller never passes, defaulting to none; a static check asserts that no controller passes it). The hook names a step and returns one SQL statement that the service executes on the same transaction at that step, then continues normally. For this test, the step is "after the first ledger entry" of a flower+sunshine task, and the statement is `SELECT 1 / (SELECT count(*) - 1 FROM wallet_ledger_entries WHERE operation_id = '<op>')`. It divides by zero only if exactly one entry of this operation already exists, so the request fails exactly after a partial write; if the hook ran at the wrong point, the statement succeeds, the request completes, and the test fails on the unexpected success. Expected: an error response, and afterwards no operation, entry, completion, or audit row for that task is visible as committed; the server-side transaction is confirmed ended (teardown evidence). Grants, triggers, and the posting code are not altered; the hook only adds one failing statement to the real transaction. This failure happens before COMMIT, so it is a confirmed rollback (Section 4.4), which is what makes the "nothing committed" assertion valid. Reconciliation: retrying the same key without the hook then settles exactly once (one operation, one completion).
- Same-key retry after a committed success (models "committed, response lost", Section 4.4): returns the replay; still one operation.
- **Real-service cap rejection (D33), a different failure from the above:** a persistent fixture couple (deterministic test emails under the reserved domain) whose recipient holds a committed flower balance of 999,999. It is created once, on first use, by a committed DO loop of ordinary operations and entries (1,001 entries of 999) through the normal chain trigger; later runs find and reuse it after checking its balance. Each run commits one new task with a 2-flower reward for that recipient and calls the real completion: expect 409 `WALLET_BALANCE_LIMIT`, no operation/entry/completion/audit for that task, balance unchanged at 999,999. Because the rejection writes nothing, the fixture stays reusable indefinitely. The sunshine cap would need 10,000 entries for an equivalent fixture; it is covered by the pure pre-check unit test and the M2-A database CHECK tests only, and this is stated as a limit.
- Listing, NULL-safe pagination (Section 3.4): one fixture couple with seven open tasks inserted with explicit timestamps: three dated tasks, two of which share both `deadline_at` and `created_at` (so `id` decides), and four undated tasks, two of which share `created_at`.
  - With limit 2 (four pages), concatenating the pages gives exactly the expected total order, each task once. The boundaries include a page ending on the last dated task (transition to undated) and pages continuing within undated tasks.
  - With limit 3, the dated-to-undated transition falls inside a page.
  - The returned cursors decode to the documented fields.
- Listing, history: a task created 40 days ago (explicit `created_at`) and completed today appears; a task that expired 31 days ago does not; the order is `history_at DESC, id DESC`.
- Listing, cursor errors: not base64url, not JSON, unknown version, an `open` cursor sent with `view=history`, an extra field, a timestamp without six fractional digits, a non-canonical uuid: each 400 with no query run. A well-formed cursor built from another couple's task returns only the caller's couple's tasks.
- **Post-commit settlement tampering (D34 ledger trigger):** complete a flower-only task normally through the real service and commit it (this also proves the deferred ledger trigger allows a normal initial settlement). Then, in separate raw transactions:
  - append an entry for the **other member** to that operation (creating that member's flower account first if needed, with chain-valid `seq` and `balance_after`), then run `SET CONSTRAINTS ALL IMMEDIATE` through the DO wrapper: expect `UT005`;
  - append an entry for the recipient's **unrewarded asset** (sunshine) to that operation and attempt a real COMMIT: the commit must fail.

  Afterwards the observer confirms that the operation still has exactly its original entry, the completion is unchanged, and both members' balances are unchanged. If the trigger were missing, the second case would commit a corrupt entry permanently; the per-run fixture contains the damage and the test reports it.
- **Existing-account lock serialization (deferred from M2):** a committed account with one committed entry; a raw transaction holds `SELECT ... FOR UPDATE`; a completion for the same recipient does not finish within a bounded wait (about 300 ms) and the observer sees a `Lock` wait; after the raw transaction rolls back, the completion finishes with `seq = previous + 1`.
- **Post-commit snapshot visibility (deferred from M2):** while a completion waits on the account lock, a raw transaction inserts and commits a valid entry (own operation with a test `source_type`, through the chain trigger); the completion's entry then builds on that committed entry's `seq` and `balance_after`.
- Lifetime sentinel for the task service callbacks after success, rejection, and SQL failure.

### 6.4 Committed fixtures, cleanup limits, and evidence

- Only on `usapp_test`, enforced by the existing `current_database()` guard before any write. Never on the main database.
- Per-run fixtures use random identifiers and a reserved email domain (proposal `@m3-fixture.test`). The D33 cap fixture uses fixed, documented identities in the same domain and is reused.
- Committed tasks, completions, wallet rows, and the users/spaces they reference cannot be deleted (append-only triggers and `ON DELETE RESTRICT`). They remain permanently. Tests filter strictly by their own identifiers and never assert global counts or empty tables. The M2-A/M2-B acceptance step "wallet tables in `usapp_test` are empty" no longer holds once M3 tests have run.
- Growth per run is small (a few users, a space, about ten tasks, fewer than twenty ledger rows); the cap fixture adds about 2,000 rows once.
- No bypass: no DELETE grants, no disabled triggers, no `SECURITY DEFINER` purge.
- Bounded waits everywhere: transaction acquisition, statements, commits, lock-wait observation, teardown polling. The `db_gated` pool has two connections; cases with a raw lock-holding transaction plus a concurrent service call must budget for both or use an additional dedicated client. The observer is a separate single-connection client and never observes its own transaction.
- Evidence limits: "nothing committed" assertions are made only for failures before COMMIT (confirmed rollbacks, Section 4.4); a failed real-COMMIT attempt is checked by observing the rows directly. Absence of rows on another connection proves only that nothing is visible as committed. Teardown polling of `pg_stat_activity`/`pg_locks` shows the server-side transaction ended and released its locks; it does not prove Drogon callback timing or pool return.
- Time budget: the deadline cases add a few seconds and the first cap-fixture creation adds a one-time cost. The 30-second `db_gated` CTest timeout must be re-checked; raising it is a test-configuration change that needs owner approval.

### 6.5 Disposable test-database reset (D29; separate owner-only procedure)

Correction to revision 1: `database/apply.ps1 -CreateDatabase` creates the database only when it does **not** exist; for an existing `usapp_test` it just applies pending migrations. It does not reset anything.

A reset is a separate, explicit, owner-authorized procedure. This design does not authorize it, and neither Claude Code nor Codex may run it:

- Destructive boundary: it drops and recreates exactly the database named `usapp_test`. It never touches `usapp` (main) or `usapp_recovery`, and it is never parameterized by a variable that could default to another name.
- Preconditions: no test runner or API instance connected to `usapp_test`; the maintenance role connected to the `postgres` maintenance database; the owner types the literal name and confirms it.
- Steps (owner only): verify the target name; `DROP DATABASE usapp_test` (with `FORCE` only if the owner accepts disconnecting sessions); `CREATE DATABASE usapp_test`; `apply.ps1 -Database usapp_test -InitRoles` (all migrations, with checksums, including frozen 0006); rerun the privilege checks; rerun the psql scripts.
- Effect: all committed fixtures, including the D33 cap fixture, are gone and are recreated on the next test run.

## 7. Rollout and acceptance

### 7.1 Proposed implementation stages (each needs its own approval and owner verification)

1. M3-A, database: `0007_tasks_one_off.sql` (tables, D34 triggers, grants), `init-roles.sql` section 8, task privilege matrix, psql test script, rollback-only db tests (Section 6.2). No API change.
2. M3-B, publish and read: `TaskService` publish/list/get, `TaskController`, contract 0.4.0 (publish/list/get, pagination, error codes), startup check extended to task tables, web list (both views, load more) and publish form with mutation guards, publish-related committed tests.
3. M3-C, completion and settlement: completion endpoint, settlement transaction with the D32 hook, committed concurrency/deadline/partial-failure/cap/serialization/visibility tests, web complete button with wallet reload.

### 7.2 Anticipated files

New: `database/migrations/0007_tasks_one_off.sql`; `database/tests/task_ledger_test.sql`; `services/api/src/services/TaskService.h/.cc`; `services/api/src/controllers/TaskController.cc`; `services/api/tests/TaskDbTests.h` and a committed-fixture helper; `apps/web/src/api/tasks.ts`, `apps/web/src/stores/tasks.ts`, `apps/web/src/views/TasksView.vue`; web specs.

Modified: `database/init-roles.sql` (new section 8 only); `database/README.md`; `contracts/openapi.yaml`; `services/api/src/core/Errors.h/.cc`; the privilege policy or a small task-table matrix used by `WalletStartupCheck` (D26); `services/api/CMakeLists.txt` (controller list); `services/api/tests/UnitTest.cc`, `StaticCheckTest.cc`, `DbGatedTest.cc`; `apps/web/src/api/types.ts`, `error-messages.ts`, `stores/session.ts`, `router/index.ts`, `views/HomeView.vue` (link); docs.

Not modified: `0000`-`0006` migrations, `init-roles.sql` section 7, `Db.h`/`Db.cc`, deployment files, dependencies, `.claude/settings.json`.

Web tests:

- `tasks` store: generation and identity guards for list loads, and for publish and completion successes and failures that settle after logout or after a switch to another account (deferred promises): no list change, no error shown, no wallet `load()` call.
- A completion success for user A that settles after switching to user B leaves B's tasks and wallet untouched.
- An applied completion triggers exactly one wallet `load()` and never writes settlement amounts into wallet balances.
- Pagination: "load more" appends without duplicates; reload restarts from page 1.
- Key lifetime: after a 503 `RESULT_UNKNOWN`, a 500, or a network error, the retry sends the same `Idempotency-Key`; after a 2xx or a 4xx business code the next action uses a new key; reset discards pending keys.
- Offline: publish and completion are blocked and not replayed.

### 7.3 Owner-run checks per stage (summary)

- Review by Codex; DryRun on `usapp_test` (only 0007 pending; 0000-0006 checksums including `2633e17e...7d543d` unchanged); apply with `-InitRoles`; privilege matrix for task and wallet tables; rerun `-InitRoles` and recheck; psql script as `usapp_api` and as the maintenance role.
- CTest: all three targets, `db_gated` not skipped; record timings against the 30 s budget.
- Web unit tests and type check; build only when deploying (dist is a live bind mount).
- Main rollout: fresh main backup **and a restore test into a scratch database before applying 0007**, because from M3-C onward the main database receives permanent, non-deletable rows; DryRun; apply 0007 with `-InitRoles`; privilege check; old runtime still works; preserve and tag the current runtime image; deploy the new runtime (startup check `ok=true`); then build and deploy web.
- Live: publish tasks with flower-only, sunshine-only, and both rewards, with and without a deadline; the partner sees them; the publisher cannot complete; the recipient completes once; a double click and a forced retry mint once; the wallet card shows the new balance after reload; the other account sees only its own balances; logging out during an in-flight completion leaves the next account's view untouched; offline completion is blocked and not replayed; an expired task cannot be completed; no-cookie and pre-auth requests 401; wrong-method codes recorded as observed.
- Database after live checks: operations, entries, and completions match the actions taken; audit has `task.created`/`task.completed` rows without free text.

### 7.4 Rollback limits

- Runtime: the M2-B image ignores task tables, so redeploying it after 0007 is safe; tasks become invisible but nothing is lost.
- Data: once real completions commit on the main database, they cannot be removed (append-only by design). Undoing them requires restoring a backup, which also discards everything written after it. Corrections belong to a later compensating-operation feature (Phase 5), not to UPDATE/DELETE.
- 0007 cannot be "unapplied" without a restore; there is no down migration. Its triggers on `wallet_operations` and `wallet_ledger_entries` act only on task-sourced operations; restoring an older runtime does not remove them, and nothing in the M2-B runtime writes such operations.

## 8. Decision table

| ID | Recommendation | Main tradeoff |
| --- | --- | --- |
| D5 | Optional deadline; Asia/Shanghai local date + optional time; date-only ends at next local midnight | Simple for users; date-only needs a clear UI label |
| D5a | Decision time read by the step-7 statement (after account locks and cap check), strictly before `deadline_at`, stored as `completed_at` | A request that waited past the deadline is rejected even if admitted in time |
| D5b | Reject past deadlines for new publishes only; claim first, so existing or concurrent successful claims replay | A duplicate may wait for the original's commit before getting an answer |
| D5c | Decision-time semantics; no commit-time re-check | A success may commit an unbounded time after the deadline; a failed tail still prevents success |
| D6 | No undo | Mistaken completions stay; undo would need reversing entries |
| D7 | Partner only, server-derived | Matches V1; no self-tasks |
| D8 | Both see all tasks and rewards | Transparency; no private tasks |
| D9 / D10 | No notify toggle, no reminders | Smaller M3; partner learns about tasks by opening the app |
| D20 | Required `Idempotency-Key` on completion | Client key handling; prevents double minting from retries |
| D22 | 0-999 flowers, 0-100,000 sunshine | Equals per-entry caps; larger single rewards impossible |
| D22a | At least one positive reward (product choice) | Simplest settlement; zero/zero tasks need the Section 3.3 schema |
| D23 | Title 1-40, description 0-500 code points | Readable cards; long notes impossible |
| D24 / D24a | Only assignee completes; same-key replay 200, other key 409 | Clear authority; clients must handle 409 |
| D25 | Idempotent publish | Extra column and header; prevents duplicate tasks on retry |
| D26 | Narrow task grants; extend startup check | More privilege surface to keep in sync; consistent with D18 |
| D27 | Reject at cap; task stays open | Users near the cap cannot complete until they spend (after M8) |
| D28 | Paginated `open` (all) and `history` (30 days by completion/expiry time) | Two requests for a full picture; status can shift between pages |
| D29 | Separate owner-only `usapp_test` reset procedure | Manual step; destroys all test fixtures including the cap fixture |
| D30 | Claim-based lock order without a task-row lock (refines plan 9.8) | Must be revisited when tasks become editable (M6) |
| D31 | Completion returns settlement facts only; client reloads `/wallet` | One extra GET; no historical balances to misapply |
| D32 | Test-only fault hook on the real settlement path | Small production-code seam; must stay unreachable from HTTP |
| D33 | Persistent reusable near-cap fixture in `usapp_test` | ~2,000 permanent rows once; sunshine cap not covered end to end |
| D34 | Database triggers bind completion, operation, and the exact entry set, revalidated on every task-entry insert (incl. scoped triggers on `wallet_operations` and `wallet_ledger_entries`) | More trigger logic and one lookup per ledger insert; adds checks to 0006 tables without editing 0006 |
| D35 | 503 `RESULT_UNKNOWN` for unconfirmed commits; same-key retry reconciles | New error code; client must keep keys after 5xx and network errors |

## 9. Decisions that block implementation

- Blocks M3-A (schema): D5, D5a (stored `completed_at` and its trigger), D22, D22a (and therefore the Section 3.1 vs 3.3 schema), D23, D25, D26, D31 (whether balance snapshot columns exist), D34.
- Blocks M3-B (publish/read): D5b (including the claim-first ordering), D7, D8, D28 (including the Section 3.4 cursor format), D35 (publish uses it too), the 0.4.0 contract shape including error codes and pagination, and D29 awareness (committed task fixtures start here and are permanent).
- Blocks M3-C (completion): D5c, D6, D20, D24, D24a, D27, D30, D32, D33, and any `db_gated` timeout change.
- D9 and D10 only confirm exclusions; they do not block.

## 10. Unverified assumptions

- `INSERT ... ON CONFLICT DO NOTHING` on `wallet_operations` (two unique indexes, one partial) waits for an in-progress conflicting insert and then skips; a later lookup in the same READ COMMITTED transaction sees the committed winner. M2-A observed the wait only for `wallet_accounts`.
- `SELECT ... FOR SHARE OF couple_members` works for `usapp_api` because that role retains UPDATE on `couple_members`.
- `(date + time) AT TIME ZONE 'Asia/Shanghai'` yields the intended instant; server-clock decisions are acceptable.
- A deferred constraint trigger on `wallet_operations` with a `WHEN (NEW.source_type = 'task')` condition, and a deferred row trigger on `wallet_ledger_entries` that returns early for non-task operations, can be added by 0007 without affecting existing 0006 behaviour or the M2-A/M2-B tests.
- A failed COMMIT caused by a deferred trigger reaches the Drogon commit callback as `false` (indistinguishable from a lost connection), which is why D35 treats it as an unknown outcome.
- A transaction holding an uncommitted `tasks` row with key K makes a concurrent `INSERT ... ON CONFLICT DO NOTHING` with the same key wait, then skip after commit or insert after rollback (the same speculative-insertion behaviour assumed for `wallet_operations`).
- The `wallet_ledger_entries` validation trigger's per-entry lookup has acceptable cost for the expected ledger volume.
- `INSERT ... ON CONFLICT DO NOTHING` on `tasks` evaluates no temporal rule (there is no temporal CHECK or insert trigger on `tasks`), so a duplicate with an identical, already validated body always reaches conflict classification (Section 4.2).
- Committed-fixture growth in `usapp_test`, including the one-time cap fixture, stays acceptable for indefinite reuse; the combined `db_gated` run stays within its timeout.
- The web client can attach a custom `Idempotency-Key` header through the existing `request()` options (the option exists; Caddy forwarding is assumed).
- The D32 hook can be added as a defaulted service argument without changing `Db.h`/`Db.cc`.

## 11. M3-A implementation status (2026-10-09): implemented; owner-run verification passed (Section 11.3)

Approved by the owner on 2026-10-09: the task database foundation, reusable task privilege checking, regression tests, and status documentation. **Claude Code built, tested, and executed nothing.** When written, M3-A was not accepted until the owner completed the steps below; the owner-run results are in Sections 11.1 and 11.3. M3-B and M3-C remain unapproved.

Files:

- New `database/migrations/0007_tasks_one_off.sql`: role preflight (also requires 0006); `tasks` (immutable, Section 3.1 positive-reward schema, approved bounds, title/description/timezone/deadline checks, publication-key uniqueness, composite membership FKs, `deadline_at` consistency with the local fields, no publish-time future-deadline rule); `task_completions` (append-only, one per task, `UNIQUE (operation_id)`, composite FKs to the task, membership and wallet operation, reward snapshot checks); statement-level append-only triggers; the immediate completion guard; the shared settlement validator and its three deferred triggers; narrowed grants. 0000-0006 are not edited.
- `database/init-roles.sql`: new section 8 after the ALL TABLES grant and after section 7 (unchanged): re-applies `SELECT, INSERT` only on both task tables for `usapp_api` and nothing for `usapp_push`/PUBLIC; skips tables that do not exist yet.
- New `services/api/src/db/TaskPrivilegePolicy.h/.cc`: task-table effective privilege matrix (same identity checks as the wallet policy; SELECT/INSERT only; no column may have UPDATE). Reuses the wallet module's row/report types, `rowsFromResult` and `safeName`; the wallet module is unchanged. **Not wired into API startup** (M3-B).
- New `database/tests/task_ledger_test.sql`: owner-run psql script (exact `usapp_test` guard first, `ON_ERROR_STOP`, `BEGIN ... ROLLBACK`, application vs maintenance mode, parenthesized CASE in IF conditions, NULL-safe constraint checks).
- Tests: new `services/api/tests/TaskLedgerDbTests.h` (block 13 of `DbGatedTest.cc`); the `db_gated` migration gate now also requires `0007_tasks_one_off`; `UnitTest.cc` task-policy case; `StaticCheckTest.cc` case for 0007, `init-roles.sql` section order, the frozen 0006 checksum, the script guard, and scope boundaries (no startup wiring, contract still 0.3.0 without task paths).
- `database/README.md`: 0007 row and notes.

Implementation details that refine this design (review these):

- Custom SQLSTATEs: `UT001` append-only; `UT002` task missing or couple mismatch; `UT003` completer is not the assignee; `UT004` reward snapshot differs; `UT005` operation missing or wrong type/source/actor/couple; `UT006` completion time not allowed (in the future, before the task's creation, or not before the deadline); `UT007` settlement validator failure. Section 3.1 had proposed a different numbering.
- The validator is named `usapp_task_settlement_validate` (Section 3.1 called it `usapp_task_settlement_valid`). It checks operation type and source, exactly one referencing completion, the completion's link back to the operation (task id, couple, actor), and the entry set: no entry for any other user, exactly one entry per positive reward asset, no entry for an unrewarded asset, sums equal to the snapshot. Counts use `count(*)` and sums are `COALESCE`d, so missing rows cannot pass silently.
- The completion guard returns early only when a key column is NULL, so that the NOT NULL constraint reports `23502`; it never lets a NULL comparison pass, because all comparisons use `IS DISTINCT FROM`.
- Title checks in the database cover length, ASCII-space trimming, C0/DEL/C1 controls and U+2028/U+2029. Trimming of other Unicode space separators is left to the service (M3-B), which is authoritative.
- `UNIQUE (operation_id)` is a backstop: the guard rejects reusing an operation for another task first (`UT005`), and 0006's business-source uniqueness rejects a second operation for the same task (`23505`).

Test coverage written (not run):

- db_gated block 13, each expected failure in its own transaction with SQLSTATE exposed through the M2-A DO wrapper, observer teardown evidence, and an explicit check that no fixture task rows are visible as committed:
  - task privilege matrix and trigger catalog;
  - valid settlements (both assets, flower-only, sunshine-only with a deadline), a non-task operation still accepted, date-only and past-deadline task inserts;
  - 18 task constraint cases plus self-assignment, non-member assignee, duplicate publication key;
  - completion guard cases: publisher completing, snapshot mismatch, missing task, couple mismatch, wrong operation type, another task's operation, publisher as operation actor, missing operation, completion at/after deadline, completion in the future, second operation for the same task, operation reused for another task;
  - validator cases: task operation without completion, missing entry for a rewarded asset, amount mismatch, extra entry for the other member, extra entry for an unrewarded asset, late append (other member and unrewarded asset) after a valid settlement already passed `SET CONSTRAINTS ALL IMMEDIATE`;
  - one real COMMIT of an invalid settlement, which must fail;
  - `usapp_api` UPDATE/DELETE/TRUNCATE on both task tables: 42501.
- psql script: the same families, plus the maintenance-role trigger rejections (`UT001`).
- unit: task-policy expectations, widened table and column grants detected, NULL and missing rows fail, safe summary.
- static: listed under Files above.

Not covered in M3-A (recorded limits):

- Every successful fixture exists only inside a rolled-back transaction. **Separate-transaction attacks on a genuinely committed completion** (appending an entry for the other member or an unrewarded asset to an already committed task operation) need committed settlements and remain an M3-C requirement; the late-append cases above are the rollback-only approximation within one transaction and are not equivalent evidence.
- No task service, endpoint, settlement transaction, concurrency, idempotent replay, deadline-across-wait, partial-failure, or real cap test: M3-B/M3-C.
- No committed near-cap fixture and no test-database reset are authorized; none were added.
- Only `db_gated` checks the task privilege matrix; API startup does not (M3-B).

Unverified assumptions specific to M3-A:

- PostgreSQL 17 accepts 0007: the regular-expression character classes with `\x`/`\u` escapes in CHECK constraints, the CHECK using `AT TIME ZONE timezone`, the constraint trigger with a `WHEN` clause on `wallet_operations`, and `%ROWTYPE` variables of the new tables.
- `apply.ps1`'s scanner accepts 0007 (distinct dollar tags; `$` only inside quoted literals).
- `SET CONSTRAINTS ALL IMMEDIATE` executed inside the M2-A DO wrapper fires deferred triggers queued earlier in the same transaction (already relied on, and passed, in M2-A).
- Adding the deferred triggers on `wallet_operations` and `wallet_ledger_entries` leaves the M2-A/M2-B db_gated cases and psql script passing (they use non-task source types).
- The added cases keep `db_gated` within its 30-second timeout.
- Everything compiles.

Owner-run acceptance steps for M3-A (one at a time, guided by Codex):

1. Codex reviews the migration, `init-roles.sql` diff, psql script, policy module and tests.
2. `apply.ps1 -DryRun` against `usapp_test`: 0000-0006 checksums match (0006 is `2633e17e...7d543d`) and only `0007_tasks_one_off` is pending; the transaction-boundary precheck passes.
3. Apply to `usapp_test` with `-InitRoles`.
4. As the maintenance role on `usapp_test`: `usapp_api` has only SELECT and INSERT on `tasks` and `task_completions`, no column has UPDATE, and `usapp_push`/`public` have nothing. The wallet matrix is unchanged (UPDATE only on `wallet_accounts.lock_token`).
5. Run `apply.ps1 -InitRoles` again (no pending migrations) and repeat step 4: unchanged.
6. Run `database/tests/task_ledger_test.sql` on `usapp_test` as `usapp_api` and as the maintenance role; each prints `ALL TASK LEDGER ASSERTIONS PASSED` with the expected mode and exits 0. Rerun `database/tests/wallet_ledger_test.sql` in both modes to confirm the new wallet-table triggers did not break it.
7. Build the backend test image and run all CTest targets: `unit`, `db_gated` (not skipped), `static_check` pass; the `db_gated` log shows both the wallet and the task privilege summaries with `ok=true`; record timings against the 30 s budget.
8. On `usapp_test`, no `tasks` or `task_completions` rows exist and no wallet rows were added by these tests.
9. Not part of M3-A: applying 0007 to the main database. That happens only in a later, separately approved rollout, after a fresh backup with a restore test (Section 7.3).

### 11.1 Owner-run M3-A results on usapp_test (2026-10-09; partial)

Run by the owner under Codex guidance. Claude Code ran none of these.

- `apply.ps1 -DryRun` against `usapp_test`: 0000-0006 checksums matched; only `0007_tasks_one_off` was pending.
- 0007 applied to `usapp_test` with `-InitRoles`, exit 0. Applied checksum of `0007_tasks_one_off`: `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`. **0007 is now applied and must never be edited** (like 0006, `2633e17e...7d543d`).
- Basic task and wallet permission query matched the expected matrix; exit 0.
- Repeated `-InitRoles`: exit 0.
- Backend test image build: exit 0.
- CTest: `unit` 0.02 s, `db_gated` 1.35 s (Passed, not skipped), `static_check` 0.01 s; total 1.38 s; all passed; exit 0. Per-assertion counts were not supplied.
- `database/tests/task_ledger_test.sql`: application mode and maintenance mode each passed 27 assertion groups, ended with `ROLLBACK`, exit 0.

Still pending when this section was written: the `wallet_ledger_test.sql` compatibility reruns in both modes (after the repair below) and the final `usapp_test` fixture checks. Both were later completed by the owner (Section 11.3).

### 11.2 Wallet SQL script compatibility repair (2026-10-09; owner-run verified, Section 11.3)

Source review found that 0007's `task_completions` foreign key references `wallet_operations`. The maintenance-mode E9 case of `database/tests/wallet_ledger_test.sql` truncates the three wallet tables together to prove the append-only trigger (`UW001`); with 0007 applied, that statement would first fail its foreign-key check (`0A000`), which proves nothing about the trigger. No failed runtime result of the wallet script has been observed; the issue was found by review only.

Repair (test script only; 0006, 0007, business code, contracts and deployment unchanged): after the existing `SET CONSTRAINTS ALL IMMEDIATE`, E9 checks `to_regclass('public.task_completions')`. If the table exists, the combined statement is `TRUNCATE wallet_ledger_entries, wallet_operations, wallet_accounts, task_completions`; otherwise the original three-table statement runs unchanged. `wallet_ledger_entries` stays first so the expected rejection remains `UW001`. No `CASCADE`, no disabled triggers, no grant changes, no assertion removed, and a foreign-key rejection is still treated as a failure. Application-mode cases, the `usapp_test` guard and the final `ROLLBACK` are unchanged.

Second predicted incompatibility, found during this repair and **not** changed (outside the "E9 only" instruction): 0007 adds a deferred constraint trigger on `wallet_ledger_entries` without a `WHEN` clause, so the script's top-level D6 ledger inserts (after `SET CONSTRAINTS ALL DEFERRED`) leave pending trigger events on that table. Maintenance-mode E8 (`TRUNCATE wallet_ledger_entries` alone) has no preceding `SET CONSTRAINTS ALL IMMEDIATE`, so PostgreSQL is expected to reject it with `55006` ("pending trigger events") instead of `UW001`, failing the maintenance run. Application mode should be unaffected because the privilege check (`42501`) comes first. This is a source-review prediction, not an observed failure.

E8 repair (authorized later on 2026-10-09; not yet run): immediately before E8's expected-failure block, the script now runs `IF NOT is_app THEN SET CONSTRAINTS ALL IMMEDIATE; END IF;`. It sits outside the exception block, so invalid fixture data fails the script directly instead of being mistaken for the expected TRUNCATE rejection. Application mode still expects `42501` and maintenance mode `UW001`; `55006` is not accepted. No `CASCADE`, disabled triggers or permission changes; the E9 conditional-table repair, the `usapp_test` guard, all assertions and the final `ROLLBACK` are unchanged; migration bytes are unchanged. The repaired wallet script remains unverified until the owner runs it in both modes.

### 11.3 Owner-run M3-A verification completed on usapp_test (2026-10-09)

Run by the owner under Codex guidance. Claude Code ran none of these. This records the complete owner-run evidence; Section 11.1 holds the earlier partial record.

- `apply.ps1 -DryRun` against `usapp_test`: 0000-0006 checksums matched; only `0007_tasks_one_off` was pending; exit 0.
- 0007 applied with `-InitRoles`, exit 0. Frozen checksum: `a9c680a03d6b426d5679672360900efdf19fb1da1cdc878779308ab470556161`.
- Basic task and wallet permission query matched; exit 0. Repeated permission initialization (`-InitRoles`): exit 0.
- Backend test image build: exit 0.
- Full CTest: `unit` Passed 0.02 s; `db_gated` Passed 1.35 s, not skipped; `static_check` Passed 0.01 s; total 1.38 s; exit 0.
- `database/tests/task_ledger_test.sql`: application and maintenance modes each passed 27 groups, ended with `ROLLBACK`, exit 0.
- Repaired `database/tests/wallet_ledger_test.sql`: application and maintenance modes each passed 28 groups, ended with `ROLLBACK`, exit 0.
- The E8/E9 compatibility repairs (Section 11.2) are now owner-run verified. Their original issues were source-review predictions, not observed failed runs.
- Final `usapp_test` query: `tasks`, `task_completions`, `wallet_accounts`, `wallet_operations` and `wallet_ledger_entries` all 0 rows; users matching the `wallet_` or `task_psql_` prefixes also 0; exit 0. This user-prefix check is not a claim that every other fixture is absent.

Limits:

- CTest ran before the E8/E9 script repairs; both repaired script modes were run successfully afterwards. No later CTest rerun is claimed.
- Per-assertion counts and the individual privilege-summary lines were not supplied.
- 0007 has not been applied to the main database; no main-database rollout was reported or is approved.
- Not verified (later stages): task API and frontend, a real settlement service, concurrency and idempotent-replay tests, and attacks on genuinely committed settlements.
- No test-database reset and no permanent cap fixture were executed.
- No separate formal acceptance decision has been recorded.

Current status: M3-A passed the recorded owner-run verification; owner Git review/commit/push is next. M3-B and M3-C implementation remain unapproved.
